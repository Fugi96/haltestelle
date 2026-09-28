"""Settings read from a TOML file. Missing keys, or a missing file, take the defaults."""

import os
import tomllib
from pathlib import Path

from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator

CONFIG_PATH = Path(
    os.environ.get("CONFIG_PATH", Path(__file__).resolve().parents[1] / "config.toml")
)


class _Section(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)


class MqttTopics(_Section):
    """Topic names, relative to the prefix."""

    departures: str = Field(default="departures", min_length=1)
    alerts: str = Field(default="alerts", min_length=1)
    power: str = Field(default="power", min_length=1)
    settings: str = Field(default="settings", min_length=1)
    status: str = Field(default="status", min_length=1)


class MqttConfig(_Section):
    """Broker connection and topics."""

    host: str = Field(default="localhost", min_length=1)
    port: int = Field(default=1883, ge=1, le=65535)
    username: str | None = None
    password: str | None = None
    prefix: str = Field(default="haltestelle", min_length=1)
    qos: int = Field(default=1, ge=0, le=2)
    reconnect_delay: float = Field(default=5, gt=0, description="Seconds between connection attempts.")
    topics: MqttTopics = MqttTopics()

    @field_validator("username", "password", mode="before")
    @classmethod
    def _empty_is_none(cls, value):
        return value or None

    def topic(self, name: str) -> str:
        """Full topic for a name in `topics`."""
        return f"{self.prefix.rstrip('/')}/{getattr(self.topics, name)}"


class Config(_Section):
    mqtt: MqttConfig = MqttConfig()


def _load() -> Config:
    raw = tomllib.loads(CONFIG_PATH.read_text()) if CONFIG_PATH.is_file() else {}
    try:
        return Config.model_validate(raw)
    except ValidationError as exc:
        raise ValueError(f"{CONFIG_PATH}: {exc}") from None


mqtt = _load().mqtt
