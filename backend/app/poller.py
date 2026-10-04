"""Polls departures for the selected station and publishes them over MQTT."""

import asyncio
import logging
import time
import traceback

from pydantic import TypeAdapter
from starlette.concurrency import run_in_threadpool

from . import db, efa, state
from .models import Alert, AlertDetail, AlertStatus, DepartureBoard
from .mqtt import ALERTS_TOPIC, DEPARTURES_TOPIC, publisher

INTERVAL = 30
LIMIT = 15
HORIZON = 45 * 60
# Quiet time after a selection change before the board goes out, so a burst of taps is one poll.
SETTLE = 2
# Allowance for one EFA request, added to a poll's start when estimating new alerts.
FETCH_TIME = 2

log = logging.getLogger(__name__)

# Published as plain alerts: the detail fields are not part of the topic.
_ALERTS = TypeAdapter(list[Alert])
_DETAILS = TypeAdapter(list[AlertDetail])

_loop: asyncio.AbstractEventLoop | None = None
_wake = asyncio.Event()
_settled_at: float | None = None
# (id, version) of the published alerts; None until the first publish.
_alert_keys: frozenset[tuple[str, int | None]] | None = None
# Set by a selection change: the next poll publishes alerts even if unchanged.
_alerts_due = False
# Alerts of the last successful poll; None until one succeeds.
_alerts: list[AlertDetail] | None = None
# Selection changes so far, and how many of them the last successful poll covers.
_changes = 0
_covered = 0
# Start of the next regular poll; None while one is under way or before the loop runs.
_next_poll: float | None = None


def build(
    station_id: str, classes: list[int] | None, chosen: list[str], limit: int = LIMIT
) -> tuple[DepartureBoard, list[AlertDetail]]:
    """Fetch departures within HORIZON, restricted to the chosen lines, and their alerts."""
    known = {line["key"]: line for line in db.station_lines(station_id)}
    # Choices whose class is filtered out are kept but not applied; none applied means all.
    ids = [
        line_id
        for key in chosen
        if (line := known.get(key)) and efa.class_included(line["cls"], classes)
        for line_id in line["ids"]
    ]
    result = efa.fetch_departures(station_id, limit, classes, within=HORIZON, lines=ids or None)
    return DepartureBoard.model_validate(result), _DETAILS.validate_python(result["alerts"])


def schedule() -> None:
    """Poll SETTLE seconds after the last call. Safe from any thread."""
    if _loop:
        _loop.call_soon_threadsafe(_restart_settle)


def _restart_settle() -> None:
    global _settled_at, _alerts_due, _changes
    _settled_at = time.monotonic() + SETTLE
    _alerts_due = True
    _changes += 1
    _wake.set()


def alert_status() -> AlertStatus:
    """Alerts of the last successful poll and when to expect newer ones. Call on the event loop."""
    now = time.monotonic()
    start = _settled_at if _settled_at is not None else _next_poll
    return AlertStatus(
        alerts=_alerts or [],
        pending=_alerts is None or _covered < _changes,
        refresh_in=round(max((start or now) - now, 0) + FETCH_TIME, 1),
    )


def _publish_alerts(alerts: list[AlertDetail], due: bool) -> None:
    """Publish alerts when the set of ids or versions changed, or when due regardless."""
    global _alert_keys
    keys = frozenset((a.id, a.version) for a in alerts)
    if not due and keys == _alert_keys:
        return
    old = _alert_keys or frozenset()
    log.info(
        "alerts: %s (added %s, removed %s)",
        ", ".join(a.id for a in alerts) or "none",
        ", ".join(sorted(f"{i} v{v}" for i, v in keys - old)) or "none",
        ", ".join(sorted(f"{i} v{v}" for i, v in old - keys)) or "none",
    )
    publisher.publish(ALERTS_TOPIC, _ALERTS.dump_json(alerts))
    _alert_keys = keys


async def _poll() -> None:
    global _alerts_due, _alerts, _covered
    station = state.get_station()
    if not station:
        return
    # Taken before the fetch, so a change arriving during it forces the next poll too.
    due, _alerts_due = _alerts_due, False
    covers = _changes
    try:
        board, alerts = await run_in_threadpool(
            build,
            station["id"],
            state.get_classes(),
            state.get_lines(station["id"]),
        )
    except Exception:  # noqa: BLE001
        # A bad response must not end the loop, nor clear the alerts.
        _alerts_due = _alerts_due or due
        traceback.print_exc()
        return
    publisher.publish(DEPARTURES_TOPIC, board.model_dump_json())
    _publish_alerts(alerts, due)
    _alerts, _covered = alerts, covers


async def run() -> None:
    """Poll every INTERVAL seconds, and SETTLE after a selection change, until cancelled."""
    global _loop, _settled_at, _next_poll
    _loop = asyncio.get_running_loop()
    _next_poll = time.monotonic()
    while True:
        # Cleared before reading the deadlines, so a change arriving now still wakes the wait.
        _wake.clear()
        now = time.monotonic()
        # A change in progress holds back the regular poll too.
        due = _settled_at if _settled_at is not None else _next_poll
        if now < due:
            try:
                await asyncio.wait_for(_wake.wait(), due - now)
            except TimeoutError:
                pass
            continue
        _settled_at = _next_poll = None
        await _poll()
        _next_poll = time.monotonic() + INTERVAL
