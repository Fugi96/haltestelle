"""MQTT publisher. Every topic is retained state, so only the latest payload per topic is kept."""

import asyncio
import logging
from threading import Lock

import aiomqtt

from .config import mqtt as cfg

log = logging.getLogger(__name__)

DEPARTURES_TOPIC = cfg.topic("departures")
ALERTS_TOPIC = cfg.topic("alerts")
POWER_TOPIC = cfg.topic("power")
SETTINGS_TOPIC = cfg.topic("settings")
STATUS_TOPIC = cfg.topic("status")


class Publisher:
    """Keeps a broker connection and publishes the latest payload per topic, retained."""

    def __init__(self) -> None:
        self._lock = Lock()
        self._latest: dict[str, bytes] = {}
        self._dirty: set[str] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._wake = asyncio.Event()

    def publish(self, topic: str, payload: str | bytes) -> None:
        """Queue a payload for topic. Safe to call from any thread."""
        if isinstance(payload, str):
            payload = payload.encode()
        with self._lock:
            self._latest[topic] = payload
            self._dirty.add(topic)
        if self._loop:
            self._loop.call_soon_threadsafe(self._wake.set)

    def _client(self) -> aiomqtt.Client:
        password = cfg.secret()
        if password is None and cfg.password_file:
            raise aiomqtt.MqttError(f"{cfg.password_file} is missing or empty")
        return aiomqtt.Client(
            hostname=cfg.host,
            port=cfg.port,
            username=cfg.username,
            password=password,
            will=aiomqtt.Will(STATUS_TOPIC, b"offline", qos=cfg.qos, retain=True),
        )

    async def run(self) -> None:
        """Publish queued payloads until cancelled, reconnecting as needed."""
        self._loop = asyncio.get_running_loop()
        while True:
            try:
                async with self._client() as client:
                    log.info("MQTT connected to %s:%s", cfg.host, cfg.port)
                    await client.publish(STATUS_TOPIC, b"online", qos=cfg.qos, retain=True)
                    # The broker may have lost retained messages while we were away.
                    with self._lock:
                        self._dirty.update(self._latest)
                    try:
                        await self._flush(client)
                    except asyncio.CancelledError:
                        await client.publish(STATUS_TOPIC, b"offline", qos=cfg.qos, retain=True)
                        raise
            except aiomqtt.MqttError as exc:
                log.warning(
                    "MQTT %s:%s: %s, retrying in %ss", cfg.host, cfg.port, exc, cfg.reconnect_delay
                )
                await asyncio.sleep(cfg.reconnect_delay)

    async def _flush(self, client: aiomqtt.Client) -> None:
        # Nothing is subscribed; iterating messages only raises on disconnect.
        gone = asyncio.ensure_future(anext(client.messages))
        try:
            while True:
                self._wake.clear()
                with self._lock:
                    batch = {t: self._latest[t] for t in self._dirty}
                for topic, payload in batch.items():
                    await client.publish(topic, payload, qos=cfg.qos, retain=True)
                    with self._lock:
                        # A newer payload queued meanwhile stays dirty.
                        if self._latest[topic] is payload:
                            self._dirty.discard(topic)
                wake = asyncio.ensure_future(self._wake.wait())
                await asyncio.wait({wake, gone}, return_when=asyncio.FIRST_COMPLETED)
                wake.cancel()
                if gone.done():
                    gone.result()
        finally:
            gone.cancel()


publisher = Publisher()
