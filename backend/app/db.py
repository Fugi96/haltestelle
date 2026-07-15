"""Local SQLite cache of stations the user has actually selected.

We deliberately do NOT preload a full station registry. Instead the cache is
built organically: whenever the user picks a station (resolved via the EFA
stopfinder), it's stored here, and from then on it shows up as a fast, offline
autocomplete suggestion. Over time this becomes exactly the set of stations
this particular household cares about.

A new connection is opened per call. FastAPI runs sync endpoints in a thread
pool, and SQLite connections aren't shareable across threads, so per-call
connections keep things simple and safe. A single write lock serialises the
(rare) writes.
"""

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
                last_used  TEXT
            )
            """
        )
        conn.execute("CREATE INDEX IF NOT EXISTS idx_stations_name ON stations(name)")


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
