"""Local SQLite store: stations the user has selected, their lines, settings.

We deliberately do NOT preload a full station registry. Instead the cache is
built organically: whenever the user picks a station (resolved via the EFA
stopfinder), it's stored here, and from then on it shows up as a fast, offline
autocomplete suggestion. Over time this becomes exactly the set of stations
this particular household cares about.

A station's lines are one row per line and direction (`line_groups`), each
with every EFA line id it runs under (`line_ids`).

A new connection is opened per call. FastAPI runs sync endpoints in a thread
pool, and SQLite connections aren't shareable across threads, so per-call
connections keep things simple and safe. A single write lock serialises the
(rare) writes.
"""

import datetime as dt
import json
import os
import sqlite3
from pathlib import Path
from threading import Lock

DB_PATH = Path(
    os.environ.get(
        "STATIONS_DB_PATH", Path(__file__).resolve().parents[2] / "data" / "stations.sqlite"
    )
)

_write_lock = Lock()


def _connect() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    """Create the stations table if it doesn't exist. Called on startup."""
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    with _connect() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS stations (
                id         TEXT PRIMARY KEY,   -- EFA global id (DHID), e.g. de:05111:18091
                name       TEXT NOT NULL,      -- full name, e.g. "Düsseldorf, Düsseldorf Hbf"
                short_name TEXT,               -- disassembledName
                use_count  INTEGER NOT NULL DEFAULT 0,
                last_used  TEXT,
                lines_generated_at TEXT        -- last fetch of its lines, UTC
            )
            """
        )
        # Added after the table existed.
        columns = {row["name"] for row in conn.execute("PRAGMA table_info(stations)")}
        if "lines_generated_at" not in columns:
            conn.execute("ALTER TABLE stations ADD COLUMN lines_generated_at TEXT")
        conn.execute("CREATE INDEX IF NOT EXISTS idx_stations_name ON stations(name)")
        # Superseded by line_groups and line_ids.
        conn.execute("DROP TABLE IF EXISTS lines")
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS line_groups (
                station_id TEXT NOT NULL,      -- stations.id
                name       TEXT NOT NULL,      -- as shown, e.g. "RE1 (RRX)"
                direction  TEXT NOT NULL,      -- H or R, from the line id
                cls        INTEGER,            -- EFA product class
                dest       TEXT,               -- label, locality prefix stripped
                dest_id    TEXT,               -- EFA stop id of dest
                last_seen  TEXT NOT NULL,      -- UTC
                PRIMARY KEY (station_id, name, direction)
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS line_ids (
                station_id TEXT NOT NULL,      -- stations.id
                id         TEXT NOT NULL,      -- EFA line id, period dropped
                name       TEXT NOT NULL,      -- line_groups.name
                direction  TEXT NOT NULL,      -- line_groups.direction
                dest       TEXT,
                dest_id    TEXT,
                last_seen  TEXT NOT NULL,      -- UTC
                PRIMARY KEY (station_id, id)
            )
            """
        )
        conn.execute("""
                     CREATE TABLE IF NOT EXISTS settings(
                        setting TEXT PRIMARY KEY, -- name of the setting
                        value   TEXT              -- value of the setting
                     )
                     """)


def upsert_station(station: dict) -> None:
    """Remember a chosen station, or bump its usage if already known.

    Most-used stations surface first in autocomplete (see `search_stations`).
    """
    with _write_lock, _connect() as conn:
        conn.execute(
            """
            INSERT INTO stations (id, name, short_name, use_count, last_used)
            VALUES (:id, :name, :short_name, 1, datetime('now'))
            ON CONFLICT(id) DO UPDATE SET
                use_count  = use_count + 1,
                last_used  = datetime('now'),
                name       = excluded.name,
                short_name = excluded.short_name
            """,
            {
                "id": station["id"],
                "name": station["name"],
                "short_name": station.get("short_name"),
            },
        )


def search_stations(query: str, limit: int = 8) -> list[dict]:
    """Substring search over locally-known stations, most-used first."""
    like = f"%{query}%"
    with _connect() as conn:
        rows = conn.execute(
            """
            SELECT id, name, short_name FROM stations
            WHERE name LIKE ? OR short_name LIKE ?
            ORDER BY use_count DESC, name ASC
            LIMIT ?
            """,
            (like, like, limit),
        ).fetchall()
    return [dict(r) for r in rows]


def read_setting(name: str):
    """A stored setting, JSON-decoded, or None if unset."""
    with _connect() as conn:
        row = conn.execute(
            "SELECT value FROM settings WHERE setting = ?", (name,)
        ).fetchone()
    return json.loads(row["value"]) if row and row["value"] is not None else None


def write_setting(name: str, value) -> None:
    """Store a setting as JSON."""
    with _write_lock, _connect() as conn:
        conn.execute(
            """
            INSERT INTO settings (setting, value) VALUES (?, ?)
            ON CONFLICT(setting) DO UPDATE SET value = excluded.value
            """,
            (name, json.dumps(value)),
        )


def line_key(name: str, direction: str) -> str:
    """Reference to one line and direction at a station."""
    return f"{name}|{direction}"


def station_lines(station_id: str) -> list[dict]:
    """Lines known at a station: key, name, direction, cls, dest, dest_id, ids."""
    with _connect() as conn:
        groups = conn.execute(
            """
            SELECT name, direction, cls, dest, dest_id FROM line_groups
            WHERE station_id = ?
            """,
            (station_id,),
        ).fetchall()
        ids = conn.execute(
            "SELECT id, name, direction FROM line_ids WHERE station_id = ? ORDER BY id",
            (station_id,),
        ).fetchall()
    by_key: dict[str, list[str]] = {}
    for row in ids:
        by_key.setdefault(line_key(row["name"], row["direction"]), []).append(row["id"])
    lines = []
    for row in groups:
        key = line_key(row["name"], row["direction"])
        lines.append({"key": key, **dict(row), "ids": by_key.get(key, [])})
    return lines


def store_lines(station_id: str, groups: list[dict], ids: list[dict]) -> None:
    """Merge lines into a station's list and mark them seen now."""
    with _write_lock, _connect() as conn:
        conn.executemany(
            """
            INSERT INTO line_groups
                (station_id, name, direction, cls, dest, dest_id, last_seen)
            VALUES (:station_id, :name, :direction, :cls, :dest, :dest_id, datetime('now'))
            ON CONFLICT(station_id, name, direction) DO UPDATE SET
                cls       = excluded.cls,
                dest      = excluded.dest,
                dest_id   = excluded.dest_id,
                last_seen = excluded.last_seen
            """,
            [{"station_id": station_id, **group} for group in groups],
        )
        conn.executemany(
            """
            INSERT INTO line_ids
                (station_id, id, name, direction, dest, dest_id, last_seen)
            VALUES (:station_id, :id, :name, :direction, :dest, :dest_id, datetime('now'))
            ON CONFLICT(station_id, id) DO UPDATE SET
                name      = excluded.name,
                direction = excluded.direction,
                dest      = excluded.dest,
                dest_id   = excluded.dest_id,
                last_seen = excluded.last_seen
            """,
            [{"station_id": station_id, **line} for line in ids],
        )


def evict_lines(station_id: str, days: int, keep: list[str]) -> None:
    """Drop lines and ids unseen for `days`, except lines whose key is in `keep`."""
    stale = "station_id = ? AND last_seen < datetime('now', ?)"
    args = (station_id, f"-{days} days")
    with _write_lock, _connect() as conn:
        groups = conn.execute(
            f"SELECT name, direction FROM line_groups WHERE {stale}", args
        ).fetchall()
        conn.executemany(
            "DELETE FROM line_groups WHERE station_id = ? AND name = ? AND direction = ?",
            [
                (station_id, row["name"], row["direction"])
                for row in groups
                if line_key(row["name"], row["direction"]) not in keep
            ],
        )
        ids = conn.execute(
            f"SELECT id, name, direction FROM line_ids WHERE {stale}", args
        ).fetchall()
        conn.executemany(
            "DELETE FROM line_ids WHERE station_id = ? AND id = ?",
            [
                (station_id, row["id"])
                for row in ids
                if line_key(row["name"], row["direction"]) not in keep
            ],
        )


def lines_generated_at(station_id: str) -> dt.datetime | None:
    """When a station's lines were last fetched, or None if never."""
    with _connect() as conn:
        row = conn.execute(
            "SELECT lines_generated_at FROM stations WHERE id = ?", (station_id,)
        ).fetchone()
    if not row or not row["lines_generated_at"]:
        return None
    return dt.datetime.fromisoformat(row["lines_generated_at"]).replace(tzinfo=dt.timezone.utc)


def mark_lines_generated(station_id: str) -> None:
    """Record that a station's lines were fetched now."""
    with _write_lock, _connect() as conn:
        conn.execute(
            "UPDATE stations SET lines_generated_at = datetime('now') WHERE id = ?",
            (station_id,),
        )
