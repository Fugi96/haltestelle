"""Polls departures for the selected station and prints them to stdout."""

import asyncio
import datetime as dt
import time
import traceback

from starlette.concurrency import run_in_threadpool

from . import db, efa, state
from .models import DepartureBoard

INTERVAL = 30
LIMIT = 40


def _clock(ts: int) -> str:
    return dt.datetime.fromtimestamp(ts).strftime("%H:%M:%S")


def _delay(seconds: int) -> str:
    sign = "-" if seconds < 0 else "+"
    return f"{sign}{abs(seconds) // 60}:{abs(seconds) % 60:02}"


def build_board(
    station_id: str, classes: list[int] | None, lines: list[dict], limit: int = LIMIT
) -> tuple[DepartureBoard, list[dict]]:
    """Fetch departures restricted to the chosen lines. Returns the pairs applied."""
    board = DepartureBoard.model_validate(
        efa.fetch_departures(station_id, limit, classes)
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
    return board, active


def format_board(
    board: DepartureBoard, name: str, classes: list[int] | None, lines: list[dict]
) -> str:
    """Render a board as a plain-text table."""
    wanted = "all" if classes is None else ",".join(map(str, classes)) or "none"
    picked = ", ".join(f"{pair['line']} {pair['dest']}" for pair in lines) or "all"
    out = [
        f"[{_clock(board.gen)}] {board.station or name} | "
        f"{len(board.departures)} departures | classes: {wanted} | lines: {picked}"
    ]
    for d in board.departures:
        out.append(
            f"  {_clock(d.ts)}  in {max(0, d.ts - board.gen) // 60:>3}m  "
            f"{_delay(d.delay) if d.rt else '  sched':>7}  "
            f"{'' if d.cls is None else d.cls:>2}  {d.line:<6} {d.dest:<28} "
            f"{('Gl. ' + d.platform) if d.platform else '':<7} {'SEV' if d.sev else ''}"
        )
    out.extend(f"  ! {alert}" for alert in board.alerts)
    return "\n".join(out)


async def run() -> None:
    """Poll every INTERVAL seconds until cancelled."""
    while True:
        started = time.monotonic()
        station = state.get_station()
        classes = state.get_classes()
        lines = state.get_lines()
        if not station:
            print("[poller] no station selected", flush=True)
        else:
            try:
                board, active = await run_in_threadpool(
                    build_board, station["id"], classes, lines
                )
                print(format_board(board, station["name"], classes, active), flush=True)
            except Exception:
                # A bad response must not end the loop.
                traceback.print_exc()
        await asyncio.sleep(max(0.0, INTERVAL - (time.monotonic() - started)))
