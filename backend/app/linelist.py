"""Per-station line list, fetched from serving lines and refreshed daily at 22:00.

Only the selected station is refreshed on schedule. Any other station is
refreshed when it is selected again and its list predates the last 22:00.
"""

import asyncio
import datetime as dt
import traceback
from collections import Counter
from zoneinfo import ZoneInfo

from starlette.concurrency import run_in_threadpool

from . import db, efa, state

TZ = ZoneInfo("Europe/Berlin")
REFRESH_AT = dt.time(22, 0)
RETRY = 30 * 60
# Lines missing from every list fetched this long are dropped, unless chosen.
KEEP_DAYS = 7


def _service_date(now: dt.datetime) -> dt.date:
    """The day the board shows next: tomorrow from REFRESH_AT on."""
    return now.date() + dt.timedelta(days=1) if now.time() >= REFRESH_AT else now.date()


def _last_refresh(now: dt.datetime) -> dt.datetime:
    """The latest REFRESH_AT at or before `now`."""
    today = dt.datetime.combine(now.date(), REFRESH_AT, tzinfo=TZ)
    return today if now >= today else today - dt.timedelta(days=1)


def is_stale(station_id: str) -> bool:
    """Whether a station's lines predate the last REFRESH_AT."""
    generated = db.lines_generated_at(station_id)
    now = dt.datetime.now(TZ)
    return generated is None or generated < _last_refresh(now)


def fetch(station_id: str, name: str) -> list[list[dict]]:
    """Fetch a station's line records, one list per day. Raises httpx.HTTPError."""
    now = dt.datetime.now(TZ)
    day = _service_date(now)
    generated = db.lines_generated_at(station_id)
    days = [day]
    # A new or long-unvisited station also gets the last weekend, for night and weekend lines.
    if generated is None or now - generated > dt.timedelta(days=KEEP_DAYS):
        saturday = day - dt.timedelta(days=(day.weekday() - 5) % 7 or 7)
        # The service day last, so its labels win.
        days = list(dict.fromkeys([saturday, saturday + dt.timedelta(days=1), day]))
    # EFA names read "Düsseldorf, Opladener Straße".
    locality = name.split(", ")[0] if ", " in name else ""
    return [efa.fetch_lines(station_id, d, locality) for d in days]


def store(station_id: str, days: list[list[dict]]) -> None:
    """Merge fetched records into the station's list and drop stale lines."""
    for records in days:
        # One label per line: the destination most of its records name.
        labels: dict[tuple, Counter] = {}
        classes = {}
        for r in records:
            key = (r["name"], r["direction"])
            labels.setdefault(key, Counter())[(r["dest"], r["dest_id"])] += 1
            classes[key] = r["cls"]
        groups = [
            {
                "name": name,
                "direction": direction,
                "cls": classes[(name, direction)],
                "dest": counts.most_common(1)[0][0][0],
                "dest_id": counts.most_common(1)[0][0][1],
            }
            for (name, direction), counts in labels.items()
        ]
        db.store_lines(station_id, groups, records)
    db.evict_lines(station_id, KEEP_DAYS, keep=state.get_lines(station_id))
    db.mark_lines_generated(station_id)


def refresh(station_id: str, name: str) -> None:
    """Fetch and store a station's lines. Raises httpx.HTTPError."""
    store(station_id, fetch(station_id, name))


async def run() -> None:
    """Keep the selected station's lines current until cancelled."""
    while True:
        now = dt.datetime.now(TZ)
        # A second past the hour, so waking early cannot find the list still current.
        delay = (_last_refresh(now) + dt.timedelta(days=1, seconds=1) - now).total_seconds()
        station = state.get_station()
        if station and is_stale(station["id"]):
            try:
                await run_in_threadpool(refresh, station["id"], station["name"])
            except Exception:
                traceback.print_exc()
                delay = min(delay, RETRY)
        await asyncio.sleep(delay)
