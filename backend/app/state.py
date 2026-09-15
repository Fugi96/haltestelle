"""In-memory store for the station currently selected for polling.

Holds the *resolved* station (a stable EFA id plus a display name), not the
raw text the user typed. The future polling loop reads this each cycle. Kept
in its own module so persistence (surviving restarts) can be added here later
— e.g. reading/writing the last selection to the SQLite DB — without touching
the API layer.
"""

from threading import Lock

_lock = Lock()
_current: dict | None = None
_classes: list[int] | None = None
_lines: list[dict] = []


def get_station() -> dict | None:
    """Return the selected station as {"id", "name"}, or None if unset."""
    with _lock:
        return dict(_current) if _current else None


def set_station(id: str, name: str) -> None:
    """Set the station to poll. Called when the user picks a candidate."""
    global _current
    with _lock:
        _current = {"id": id, "name": name}


def get_classes() -> list[int] | None:
    """Return the EFA classes to include, or None for all."""
    with _lock:
        return list(_classes) if _classes is not None else None


def set_classes(classes: list[int] | None) -> None:
    """Set the EFA classes to include. None or empty clears the filter."""
    global _classes
    with _lock:
        _classes = sorted(set(classes)) if classes else None


def get_lines() -> list[dict]:
    """Return the chosen {"line", "dest"} pairs. Empty means all."""
    with _lock:
        return [dict(pair) for pair in _lines]


def set_lines(lines: list[dict]) -> None:
    """Set the {"line", "dest"} pairs to include. Empty clears the filter."""
    global _lines
    with _lock:
        unique = dict.fromkeys((pair["line"], pair["dest"]) for pair in lines)
        _lines = [{"line": line, "dest": dest} for line, dest in unique]
