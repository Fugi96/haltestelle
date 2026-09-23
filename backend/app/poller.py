"""Polls departures for the selected station and publishes them over MQTT."""

import asyncio
import time
import traceback

from starlette.concurrency import run_in_threadpool

from . import db, efa, state
from .mqtt import DEPARTURES_TOPIC, publisher
from .models import DepartureBoard

INTERVAL = 30
LIMIT = 40
HORIZON = 45 * 60


def build_board(
    station_id: str, classes: list[int] | None, lines: list[dict], limit: int = LIMIT
) -> DepartureBoard:
    """Fetch departures within HORIZON, restricted to the chosen lines."""
    board = DepartureBoard.model_validate(
        efa.fetch_departures(station_id, limit, classes, within=HORIZON)
    )
    known = {(p["line"], p["dest"]): p["cls"] for p in db.station_lines(station_id)}
    # Choices whose class is filtered out are kept but not applied; none applied means all.
    active = [
        pair
        for pair in lines
        if (key := (pair["line"], pair["dest"])) in known
        and efa.class_included(known[key], classes)
    ]
    if active:
        wanted = {(pair["line"], pair["dest"]) for pair in active}
        kept = [d for d in board.departures if (d.line, d.dest) in wanted]
        board = board.model_copy(update={"departures": kept})
    return board


async def run() -> None:
    """Poll every INTERVAL seconds until cancelled."""
    while True:
        started = time.monotonic()
        station = state.get_station()
        if station:
            try:
                board = await run_in_threadpool(
                    build_board, station["id"], state.get_classes(), state.get_lines()
                )
                publisher.publish(DEPARTURES_TOPIC, board.model_dump_json())
            except Exception:
                # A bad response must not end the loop.
                traceback.print_exc()
        await asyncio.sleep(max(0.0, INTERVAL - (time.monotonic() - started)))
