"""Haltestelle backend.

Lets a user pick which station to poll departures for. Discovery is organic:
the user searches, the EFA stopfinder returns candidate stops, the user picks
one, and that choice is cached locally so it appears as autocomplete next
time. A background poller (added later) will read the selected station via
`state.get_station()` and publish departures over MQTT.

Endpoints:
    GET  /                     -> the single-page frontend
    GET  /assets/*             -> frontend images
    GET  /api/stations?q=      -> autocomplete over locally-known stations
    GET  /api/stations/lookup  -> online lookup via EFA (adds nothing itself)
    POST /api/station          -> select a station (sets current + caches it)
    GET  /api/station          -> the currently selected station
    GET  /api/departures       -> departures for the selected station

Run (from the `backend/` directory):

    uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
"""

import os
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles

from . import db, efa, state
from .models import DepartureBoard, StationCandidate, StationSelection, StationState

# Where the frontend files live. Defaults to the sibling `frontend/` folder in
# the repo, but is overridable via env var so the path can differ in Docker.
FRONTEND_DIR = Path(
    os.environ.get("FRONTEND_DIR", Path(__file__).resolve().parents[2] / "frontend")
)


@asynccontextmanager
async def lifespan(app: FastAPI):
    db.init_db()
    yield


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
    db.upsert_station(selection.model_dump())
    state.set_station(selection.id, selection.name)
    return StationState(**(state.get_station() or {}))


@app.get("/api/station", response_model=StationState)
def read_station():
    """Return the station currently selected for polling."""
    current = state.get_station()
    return StationState(**current) if current else StationState()


@app.get("/api/departures", response_model=DepartureBoard)
def read_departures(limit: int = Query(8, ge=1, le=30)):
    """Departures for the currently selected station.

    Same payload the poller will publish, so it can be inspected directly.
    """
    current = state.get_station()
    if not current:
        raise HTTPException(status_code=409, detail="No station selected.")
    try:
        return efa.fetch_departures(current["id"], limit=limit)
    except httpx.HTTPError as exc:
        raise HTTPException(status_code=502, detail=f"Departure lookup failed: {exc}")
