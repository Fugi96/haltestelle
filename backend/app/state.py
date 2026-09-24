"""Selected station, vehicle classes and chosen lines, persisted in SQLite.

Holds the *resolved* station (a stable EFA id plus a display name), not the
raw text the user typed. Chosen lines are kept per station, so switching back
restores them.
"""

from threading import Lock

from . import db

# Chosen lines of all stations share one setting, so updates must not interleave.
_lines_lock = Lock()


def get_station() -> dict | None:
    """Return the selected station as {"id", "name"}, or None if unset."""
    return db.read_setting("station")


def set_station(id: str, name: str) -> None:
    """Set the station to poll. Called when the user picks a candidate."""
    db.write_setting("station", {"id": id, "name": name})


def get_classes() -> list[int] | None:
    """Return the EFA classes to include, or None for all."""
    return db.read_setting("classes")


def set_classes(classes: list[int] | None) -> None:
    """Set the EFA classes to include. None or empty clears the filter."""
    db.write_setting("classes", sorted(set(classes)) if classes else None)


def get_lines(station_id: str) -> list[str]:
    """Return the keys of the lines chosen at a station. Empty means all."""
    return (db.read_setting("lines") or {}).get(station_id, [])


def set_lines(station_id: str, keys: list[str]) -> None:
    """Set the keys of the lines to include at a station. Empty clears the filter."""
    with _lines_lock:
        chosen = db.read_setting("lines") or {}
        chosen[station_id] = list(dict.fromkeys(keys))
        db.write_setting("lines", chosen)
