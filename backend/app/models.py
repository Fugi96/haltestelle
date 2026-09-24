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


class ClassFilter(BaseModel):
    """Vehicle classes departures are restricted to."""

    classes: list[int] | None = Field(
        None, description="EFA product.class values to include. Null for all."
    )


class Power(BaseModel):
    """Whether output is switched on."""

    on: bool = True


class OutputSettings(BaseModel):
    """Output settings."""

    brightness: int = Field(100, ge=0, le=100, description="Brightness in percent.")


class KnownLine(BaseModel):
    """A line in one direction at a station."""

    key: str = Field(description="Reference used to choose the line.")
    name: str = Field(description='Line as shown, e.g. "RE1 (RRX)".')
    direction: str = Field(description="H or R, from the EFA line id.")
    cls: int | None = Field(None, description="EFA product class, e.g. 2 for U-Bahn.")
    dest: str | None = Field(None, description="Destination, locality prefix stripped.")
    dest_id: str | None = Field(None, description="EFA stop id of the destination.")
    ids: list[str] = Field([], description="EFA line ids it runs under, period dropped.")


class LineSelection(BaseModel):
    """Lines departures are restricted to."""

    lines: list[str] = Field([], description="Keys of the lines to include. Empty for all.")


class Departure(BaseModel):
    """One upcoming departure."""

    line: str = Field(description='Line number as shown, e.g. "U72".')
    cls: int | None = Field(None, description="EFA product class, e.g. 2 for U-Bahn.")
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
