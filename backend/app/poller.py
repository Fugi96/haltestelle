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


async def run() -> None:
    """Poll every INTERVAL seconds until cancelled."""
    while True:
        started = time.monotonic()
        station = state.get_station()
        if station:
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
        await asyncio.sleep(max(0.0, INTERVAL - (time.monotonic() - started)))
