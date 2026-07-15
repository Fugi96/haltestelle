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
