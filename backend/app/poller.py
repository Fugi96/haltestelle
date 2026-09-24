"""Polls departures for the selected station and publishes them over MQTT."""

import asyncio
import time
import traceback

from starlette.concurrency import run_in_threadpool

from . import db, efa, state
from .mqtt import DEPARTURES_TOPIC, publisher
from .models import DepartureBoard

INTERVAL = 30
LIMIT = 15
HORIZON = 45 * 60
# Quiet time after a selection change before the board goes out, so a burst of taps is one poll.
SETTLE = 2

_loop: asyncio.AbstractEventLoop | None = None
_wake = asyncio.Event()
_settled_at: float | None = None


def build_board(
    station_id: str, classes: list[int] | None, chosen: list[str], limit: int = LIMIT
) -> DepartureBoard:
    """Fetch departures within HORIZON, restricted to the chosen lines."""
    known = {line["key"]: line for line in db.station_lines(station_id)}
    # Choices whose class is filtered out are kept but not applied; none applied means all.
    ids = [
        line_id
        for key in chosen
        if (line := known.get(key)) and efa.class_included(line["cls"], classes)
        for line_id in line["ids"]
    ]
    return DepartureBoard.model_validate(
        efa.fetch_departures(station_id, limit, classes, within=HORIZON, lines=ids or None)
    )


def schedule() -> None:
    """Poll SETTLE seconds after the last call. Safe from any thread."""
    if _loop:
        _loop.call_soon_threadsafe(_restart_settle)


def _restart_settle() -> None:
    global _settled_at
    _settled_at = time.monotonic() + SETTLE
    _wake.set()


async def _poll() -> None:
    station = state.get_station()
    if not station:
        return
    try:
        board = await run_in_threadpool(
            build_board,
            station["id"],
            state.get_classes(),
            state.get_lines(station["id"]),
        )
        publisher.publish(DEPARTURES_TOPIC, board.model_dump_json())
    except Exception:
        # A bad response must not end the loop.
        traceback.print_exc()


async def run() -> None:
    """Poll every INTERVAL seconds, and SETTLE after a selection change, until cancelled."""
    global _loop, _settled_at
    _loop = asyncio.get_running_loop()
    next_poll = time.monotonic()
    while True:
        # Cleared before reading the deadlines, so a change arriving now still wakes the wait.
        _wake.clear()
        now = time.monotonic()
        # A change in progress holds back the regular poll too.
        due = _settled_at if _settled_at is not None else next_poll
        if now < due:
            try:
                await asyncio.wait_for(_wake.wait(), due - now)
            except asyncio.TimeoutError:
                pass
            continue
        _settled_at = None
        await _poll()
        next_poll = time.monotonic() + INTERVAL
