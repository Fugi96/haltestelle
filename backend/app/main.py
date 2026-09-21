"""Haltestelle backend.

Lets a user pick which station to poll departures for. Discovery is organic:
the user searches, the EFA stopfinder returns candidate stops, the user picks
one, and that choice is cached locally so it appears as autocomplete next
time. A background poller (`poller.py`) fetches departures for the selected
station and vehicle classes.

Endpoints:
    GET  /                     -> the single-page frontend
    GET  /assets/*             -> frontend images
    GET  /api/stations?q=      -> autocomplete over locally-known stations
    GET  /api/stations/lookup  -> online lookup via EFA (adds nothing itself)
    POST /api/station          -> select a station (sets current + caches it)
    GET  /api/station          -> the currently selected station
    GET  /api/station/lines    -> line + destination pairs known at that station
    GET  /api/classes          -> the vehicle classes departures are filtered to
    PUT  /api/classes          -> set those classes (null for all)
    GET  /api/lines            -> the line + destination pairs to include
    PUT  /api/lines            -> set those pairs (empty for all)
    GET  /api/power            -> whether output is on
    PUT  /api/power            -> switch output on or off
    GET  /api/settings         -> output settings (brightness)
    PUT  /api/settings         -> store output settings
    GET  /api/departures       -> departures for the selected station

Run (from the `backend/` directory):

    uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
"""

import asyncio
import os
from contextlib import asynccontextmanager, suppress
from pathlib import Path

import httpx
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles

from . import db, efa, mqtt, poller, state
from .models import (
    ClassFilter,
    DepartureBoard,
    KnownLine,
    LineSelection,
    OutputSettings,
    Power,
    StationCandidate,
    StationSelection,
    StationState,
)

# Where the frontend files live. Defaults to the sibling `frontend/` folder in
# the repo, but is overridable via env var so the path can differ in Docker.
FRONTEND_DIR = Path(
    os.environ.get("FRONTEND_DIR", Path(__file__).resolve().parents[2] / "frontend")
)

# Departures scanned to learn a station's lines. Lines absent from them stay unknown.
LINE_SEED_LIMIT = 300


@asynccontextmanager
async def lifespan(app: FastAPI):
    db.init_db()
    _publish_power(_stored(Power))
    _publish_settings(_stored(OutputSettings))
    tasks = [asyncio.create_task(mqtt.publisher.run()), asyncio.create_task(poller.run())]
    yield
    for task in reversed(tasks):
        task.cancel()
        with suppress(asyncio.CancelledError):
            await task


app = FastAPI(title="Haltestelle", version="0.2.0", lifespan=lifespan)

# Images referenced by the frontend. Guarded so a missing folder doesn't stop
# the API from starting.
if (FRONTEND_DIR / "assets").is_dir():
    app.mount(
        "/assets", StaticFiles(directory=FRONTEND_DIR / "assets"), name="assets"
    )


@app.get("/")
def serve_frontend():
    """Serve the single-page frontend."""
    index = FRONTEND_DIR / "index.html"
    if index.is_file():
        return FileResponse(index)
    return HTMLResponse(
        "<h1>Haltestelle</h1><p>Frontend not found.</p>", status_code=200
    )


@app.get("/api/stations", response_model=list[StationCandidate])
def autocomplete(q: str = Query("")):
    """Typeahead over locally-known stations only — no external calls.

    This is what the frontend hits on every keystroke, so it must stay cheap
    and offline. New stations enter the cache via `set_station`.
    """
    q = q.strip()
    if not q:
        return []
    return db.search_stations(q)


@app.get("/api/stations/lookup", response_model=list[StationCandidate])
def lookup(q: str = Query(..., min_length=1)):
    """Resolve free text via the EFA stopfinder and return candidate stops.

    Used when the wanted station isn't in the local cache yet. Deliberately
    does NOT save anything — the user picks one via POST /api/station, which is
    what commits it to the cache.
    """
    try:
        return efa.search_locations(q.strip())
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"Station lookup failed: {exc}")


@app.post("/api/station", response_model=StationState)
def select_station(selection: StationSelection):
    """Set the current station and remember it for future autocomplete.

    This is the single "command" today. If more actions arrive later (clear
    display, change interval, …), a small command dispatcher would slot in here.
    """
    # Learned once per station. Fetched before saving, so a failure changes nothing.
    new_lines = []
    if not db.station_lines(selection.id):
        try:
            board = efa.fetch_departures(selection.id, limit=LINE_SEED_LIMIT)
        except httpx.HTTPError as exc:
            raise HTTPException(status_code=502, detail=f"Line lookup failed: {exc}")
        new_lines = [
            {"line": d["line"], "dest": d["dest"], "cls": d["cls"]}
            for d in board["departures"]
        ]

    db.upsert_station(selection.model_dump())
    db.add_lines(selection.id, new_lines)
    # Chosen lines belong to one station.
    if (state.get_station() or {}).get("id") != selection.id:
        state.set_lines([])
    state.set_station(selection.id, selection.name)
    return StationState(**(state.get_station() or {}))


@app.get("/api/station", response_model=StationState)
def read_station():
    """Return the station currently selected for polling."""
    current = state.get_station()
    return StationState(**current) if current else StationState()


@app.get("/api/station/lines", response_model=list[KnownLine])
def read_station_lines():
    """Line + destination pairs known at the selected station."""
    current = state.get_station()
    if not current:
        raise HTTPException(status_code=409, detail="No station selected.")
    return db.station_lines(current["id"])


@app.get("/api/classes", response_model=ClassFilter)
def read_classes():
    """Return the vehicle classes departures are filtered to."""
    return ClassFilter(classes=state.get_classes())


@app.put("/api/classes", response_model=ClassFilter)
def select_classes(selection: ClassFilter):
    """Set the vehicle classes to include. Null clears the filter."""
    state.set_classes(selection.classes)
    return ClassFilter(classes=state.get_classes())


@app.get("/api/lines", response_model=LineSelection)
def read_lines():
    """Return the line + destination pairs departures are restricted to."""
    return LineSelection.model_validate({"lines": state.get_lines()})


@app.put("/api/lines", response_model=LineSelection)
def select_lines(selection: LineSelection):
    """Set the line + destination pairs to include. Empty clears the filter.

    Pairs not known at the selected station are dropped.
    """
    current = state.get_station()
    known = (
        {(p["line"], p["dest"]) for p in db.station_lines(current["id"])}
        if current
        else set()
    )
    state.set_lines(
        [pair.model_dump() for pair in selection.lines if (pair.line, pair.dest) in known]
    )
    return LineSelection.model_validate({"lines": state.get_lines()})


def _stored(model):
    """A settings model read from SQLite. Unset fields take their defaults."""
    stored = {
        name: value
        for name in model.model_fields
        if (value := db.read_setting(name)) is not None
    }
    return model.model_validate(stored)


def _store(settings) -> None:
    for name, value in settings.model_dump().items():
        db.write_setting(name, value)


def _publish_power(power: Power) -> None:
    mqtt.publisher.publish(mqtt.POWER_TOPIC, "on" if power.on else "off")


def _publish_settings(settings: OutputSettings) -> None:
    mqtt.publisher.publish(mqtt.SETTINGS_TOPIC, settings.model_dump_json())


@app.get("/api/power", response_model=Power)
def read_power():
    """Return whether output is switched on."""
    return _stored(Power)


@app.put("/api/power", response_model=Power)
def write_power(power: Power):
    """Switch output on or off. Persisted in SQLite and published."""
    _store(power)
    _publish_power(power)
    return power


@app.get("/api/settings", response_model=OutputSettings)
def read_settings():
    """Return the output settings. Unset ones take their defaults."""
    return _stored(OutputSettings)


@app.put("/api/settings", response_model=OutputSettings)
def write_settings(settings: OutputSettings):
    """Store the output settings. Persisted in SQLite and published as a whole."""
    _store(settings)
    _publish_settings(settings)
    return settings


@app.get("/api/departures", response_model=DepartureBoard)
def read_departures(limit: int = Query(poller.LIMIT, ge=1, le=200)):
    """Departures for the selected station, classes and lines.

    Built exactly as the poller builds them, so they can be inspected directly.
    """
    current = state.get_station()
    if not current:
        raise HTTPException(status_code=409, detail="No station selected.")
    try:
        return poller.build_board(
            current["id"], state.get_classes(), state.get_lines(), limit
        )
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"Departure lookup failed: {exc}")
