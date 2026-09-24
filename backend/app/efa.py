"""Client for the EFA (Elektronische Fahrplanauskunft) journey-planning API.

For now this points at the VRR OpenService test server, which covers the
Rhein-Ruhr region (including Düsseldorf) and needs no registration. The base
URL is overridable via EFA_BASE_URL so you can swap in a registered production
endpoint (or a different Verkehrsverbund) later without code changes.

Three calls live here: the "stopfinder" (name -> candidate stops), the
departure monitor (stop id -> upcoming departures) and serving lines (stop
id -> the lines running there), all keyed by the stopfinder's `id` values.

Both are synchronous, like the rest of the app. Async callers should use
`run_in_threadpool`.
"""

import datetime as dt
import html
import json
import os
import re
import time

import httpx

EFA_BASE_URL = os.environ.get(
    "EFA_BASE_URL", "https://openservice-test.vrr.de/openservice"
)
_TIMEOUT = httpx.Timeout(10.0)


def search_locations(query: str, limit: int = 8) -> list[dict]:
    """Resolve free text to candidate stops via XML_STOPFINDER_REQUEST.

    Returns a list of dicts with id / name / short_name, best match first.
    Raises httpx.HTTPError on network or HTTP failures (handled by the caller).
    """
    params = {
        "outputFormat": "rapidJSON",
        "type_sf": "any",
        "name_sf": query,
    }
    resp = httpx.get(
        f"{EFA_BASE_URL}/XML_STOPFINDER_REQUEST", params=params, timeout=_TIMEOUT
    )
    resp.raise_for_status()
    data = resp.json()

    stops = []
    for loc in data.get("locations", []):
        # Skip streets, POIs and localities — we only ever poll actual stops.
        if loc.get("type") != "stop":
            continue
        stops.append(
            {
                "id": loc.get("id"),
                "name": loc.get("name"),
                "short_name": loc.get("disassembledName"),
                "match_quality": loc.get("matchQuality") or 0,
                "is_best": bool(loc.get("isBest")),
            }
        )

    # Best match first, then by descending match quality.
    stops.sort(key=lambda s: (not s["is_best"], -s["match_quality"]))
    return stops[:limit]


# --- departure monitor -------------------------------------------------------

# UTC with a trailing Z. The response header's `serverTime` is local instead.
_EFA_TIME = "%Y-%m-%dT%H:%M:%SZ"

# The stop's own locality, as EFA abbreviates it in "D-Opladener Straße".
_LOCALITY_ABBREV = re.compile(r"^([A-ZÄÖÜ]{1,3})-")

# Designators that only name a stop together with their locality.
_GENERIC_NAMES = {"Hbf", "Hauptbahnhof", "Bf", "Bahnhof", "Btf", "Betriebshof", "ZOB"}

# Some infoLinks carry only a link label ("Hier alle Infos"), not real text.
_MIN_ALERT_LEN = 40

# Including any of these also returns class 0, Ersatzverkehr.
_RAIL_CLASSES = {13, 14, 15, 16}

# Long-distance trains: serving lines lists them once per train number.
_LONG_DISTANCE = {14, 15, 16}


def class_included(cls: int | None, classes: list[int] | None) -> bool:
    """Whether departures of class `cls` come back under this class filter."""
    if classes is None:
        return True
    return cls in classes or (cls == 0 and not _RAIL_CLASSES.isdisjoint(classes))


def _epoch(stamp: str) -> int:
    """EFA UTC timestamp -> epoch seconds."""
    return int(
        dt.datetime.strptime(stamp, _EFA_TIME)
        .replace(tzinfo=dt.timezone.utc)
        .timestamp()
    )


def _clean_name(name: str, locality: str = "", abbrev: str = "") -> str:
    """Drops the polled stop's own locality; other towns stay, as in "Ratingen Mitte"."""

    def cut(rest: str) -> str:
        # What is left has to name a stop: "Langenfeld (Rheinl) S" would become "S",
        # "Düsseldorf Hbf" would become "Hbf", and neither is a name.
        rest = rest.strip()
        return name.strip() if len(rest) < 3 or rest in _GENERIC_NAMES else rest

    for prefix in (f"{locality}, ", f"{locality} ") if locality else ():
        if name.startswith(prefix):
            return cut(name[len(prefix) :])

    match = _LOCALITY_ABBREV.match(name)
    # "D-" at a Düsseldorf stop, but not "DU-" (Duisburg), whose letters differ.
    if match and (
        match.group(1) == abbrev or locality.lower().startswith(match.group(1).lower())
    ):
        return cut(name[match.end() :])
    return name.strip()


def _locality_of(event: dict) -> tuple[str, str]:
    """The stop's town and the abbreviation EFA prefixes its stop names with."""
    parent = event["location"]["parent"]
    locality = (parent.get("parent") or {}).get("name") or ""
    match = _LOCALITY_ABBREV.match(parent.get("name") or "")
    return locality, match.group(1) if match else ""


def _line_name(transport: dict) -> str:
    """Line as shown. The Wuppertal Schwebebahn has no `number`, only "60" in disassembledName."""
    return (
        transport.get("number")
        or transport.get("disassembledName")
        or transport.get("name")
        or ""
    )


def line_id(full_id: str) -> str:
    """An EFA line id without its timetable period, which changes every year.

    "rbg:70076: :R:j26" -> "rbg:70076: :R". The departure monitor's `line=`
    accepts this form.
    """
    parts = full_id.split(":")
    return ":".join(parts[:4]) if len(parts) == 5 else full_id


def _strip_html(text: str) -> str:
    """Flatten EFA's HTML disruption text into one plain-text line.

    The `content` fields carry <div> wrappers, <br /> breaks and named
    entities (&uuml;, &bdquo;).
    """
    text = re.sub(r"<br\s*/?>", " ", text, flags=re.IGNORECASE)
    text = re.sub(r"</p\s*>", " ", text, flags=re.IGNORECASE)
    text = re.sub(r"<[^>]+>", "", text)
    return re.sub(r"\s+", " ", html.unescape(text)).strip()


def fetch_departures(
    station_id: str,
    limit: int = 8,
    classes: list[int] | None = None,
    within: int | None = None,
    lines: list[str] | None = None,
) -> dict:
    """Fetch upcoming departures for a stop, trimmed to the useful fields.

    `station_id` is an EFA global id (DHID) as returned by `search_locations`.
    `classes` restricts to those EFA product classes and `lines` to those line
    ids; None means all. `limit` counts after both filters. `within` drops
    departures more than that many seconds away; EFA itself has no time filter.

    The raw response runs ~30 kB for eight departures, mostly nested location
    records and HTML notices repeated per event; this reduces it to roughly
    1 kB. `ts` and `delay` are both epoch seconds — EFA resolves to 30s at
    best, and seconds stay within a 32-bit int.

    Raises httpx.HTTPError on network or HTTP failures.
    """
    # No includedMeans at all would mean every class, the opposite of empty.
    if classes is not None and not classes:
        return {"station": None, "gen": int(time.time()), "departures": [], "alerts": []}

    params = {
        "outputFormat": "rapidJSON",
        "mode": "direct",
        "type_dm": "stop",
        "name_dm": station_id,
        "depType": "stopEvents",
        "useRealtime": "1",
        "limit": str(limit),
        "itdDate": dt.date.today().strftime("%Y%m%d"),
    }
    if classes is not None:
        # Must repeat the parameter; "4,5" matches nothing.
        params["includedMeans"] = [str(c) for c in classes] # type: ignore
    if lines:
        params["line"] = lines # type: ignore
    resp = httpx.get(f"{EFA_BASE_URL}/XML_DM_REQUEST", params=params, timeout=_TIMEOUT)
    resp.raise_for_status()
    events = resp.json().get("stopEvents") or []

    departures = []
    # Keyed by info id so the same notice, repeated on every event, lands once.
    alerts: dict[str, str] = {}
    locality, abbrev = _locality_of(events[0]) if events else ("", "")

    for event in events:
        transport = event["transportation"]
        planned = event["departureTimePlanned"]
        # Only set when the trip is realtime-controlled. Absent for a whole
        # stop during engineering works, and for cross-operator services.
        estimated = event.get("departureTimeEstimated")

        # "SEV" (Schienenersatzverkehr) means the vehicle is a bus even though
        # `transportation` still names the tram line.
        hints = " ".join(h.get("content", "") for h in event.get("hints", []))

        departures.append(
            {
                "line": _line_name(transport),
                "cls": transport.get("product", {}).get("class"),
                "dest": _clean_name(transport["destination"]["name"], locality, abbrev),
                "ts": _epoch(estimated or planned),
                "delay": _epoch(estimated) - _epoch(planned) if estimated else 0,
                "rt": estimated is not None,
                "platform": event["location"].get("properties", {}).get("platform"),
                "sev": "SEV" in hints,
            }
        )

        for info in event.get("infos", []):
            for link in info.get("infoLinks", []):
                text = _strip_html(link.get("content") or link.get("subtitle") or "")
                if len(text) >= _MIN_ALERT_LEN:
                    alerts.setdefault(info.get("id") or text, text)

    # Realtime estimates reorder departures and EFA does not re-sort.
    departures.sort(key=lambda d: d["ts"])

    if within is not None:
        horizon = time.time() + within
        departures = [d for d in departures if d["ts"] <= horizon]

    return {
        "station": _clean_name(events[0]["location"]["parent"]["name"], locality, abbrev)
        if events
        else None,
        # Reference point for countdowns, and for spotting stale data.
        "gen": int(time.time()),
        "departures": departures,
        "alerts": list(alerts.values()),
    }


# --- serving lines -----------------------------------------------------------


def fetch_lines(station_id: str, date: dt.date, locality: str = "") -> list[dict]:
    """Lines running at a stop on `date`, via XML_SERVINGLINES_REQUEST.

    One dict per record: id (see `line_id`), name, direction, cls, dest and
    dest_id. The ids are the departure monitor's. Long-distance trains are left
    out. `locality` is the stop's town, stripped from destinations as on the
    board. Raises httpx.HTTPError on network or HTTP failures.
    """
    params = {
        "outputFormat": "rapidJSON",
        "mode": "odv",
        "type_sl": "stopID",
        "name_sl": station_id,
        "lineReqType": "1",
        "lsShowTrainsExplicit": "1",
        # Undocumented. Without it lineReqType=1 has no RE/RB, whatever lsShowTrainsExplicit says.
        "withoutTrains": "0",
        "itdDate": date.strftime("%Y%m%d"),
    }
    resp = httpx.get(
        f"{EFA_BASE_URL}/XML_SERVINGLINES_REQUEST", params=params, timeout=_TIMEOUT
    )
    resp.raise_for_status()
    # Serving-lines responses have carried stray Latin-1 bytes.
    data = json.loads(resp.content.decode("utf-8", errors="replace"))

    lines = []
    for record in data.get("lines") or []:
        cls = (record.get("product") or {}).get("class")
        name = _line_name(record)
        parts = record["id"].split(":")
        if cls in _LONG_DISTANCE or not name.strip() or len(parts) < 4:
            continue
        destination = record.get("destination") or {}
        lines.append(
            {
                "id": line_id(record["id"]),
                "name": name,
                "direction": parts[3],
                "cls": cls,
                "dest": _clean_name(destination.get("name") or "", locality) or None,
                "dest_id": destination.get("id"),
            }
        )
    return lines
