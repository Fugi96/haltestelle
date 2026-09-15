"""Pydantic schemas for request bodies and responses."""

from pydantic import BaseModel, Field


class StationCandidate(BaseModel):
    """A station returned by autocomplete or online lookup."""

    id: str
    name: str
    short_name: str | None = None


class StationSelection(BaseModel):
    """Request body for choosing the station to poll."""

    id: str = Field(min_length=1, description="EFA global id (DHID) of the stop.")
    name: str = Field(min_length=1, description="Display name of the stop.")
    short_name: str | None = None


class StationState(BaseModel):
    """The station currently selected for polling."""

    id: str | None = None
    name: str | None = None


class Departure(BaseModel):
    """One upcoming departure."""

    line: str = Field(description='Line number as shown, e.g. "U72".')
    dest: str = Field(description="Destination, locality prefix stripped.")
    ts: int = Field(description="Departure time, epoch seconds. Realtime if rt.")
    delay: int = Field(0, description="Seconds behind schedule.")
    rt: bool = Field(False, description="Whether ts is realtime or timetable only.")
    platform: str | None = None
    sev: bool = Field(False, description="Replacement service — a bus, not a tram.")


class DepartureBoard(BaseModel):
    """Departures for a station."""

    station: str | None = None
    gen: int = Field(description="Build time, epoch seconds.")
    departures: list[Departure] = []
    alerts: list[str] = Field([], description="Disruption notices, as plain text.")
