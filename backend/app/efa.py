"""Client for the EFA (Elektronische Fahrplanauskunft) journey-planning API.

For now this points at the VRR OpenService test server, which covers the
Rhein-Ruhr region (including Düsseldorf) and needs no registration. The base
URL is overridable via EFA_BASE_URL so you can swap in a registered production
endpoint (or a different Verkehrsverbund) later without code changes.

Only the "stopfinder" (name -> candidate stops) is implemented here. The
departure query (XML_DM_REQUEST) will live alongside it when the poller is
built; it will reuse the `id` values this returns.
"""

import os

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
