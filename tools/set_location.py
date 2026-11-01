"""Sets where the radar is centred and builds the map around that position.

The radar position is the one thing this project does not ship: you choose it. This tool writes it
to include/user_config.h and builds the matching map into include/map_local.inc, from OpenStreetMap.
Both files are git-ignored, so your position never ends up in a commit.

  python -I tools/set_location.py --lat 52.3731 --lon 4.8922 --tz "CET-1CEST,M3.5.0,M10.5.0/3"

Run it again with other numbers to move the radar, or without arguments to rebuild the map for the
position that is already in include/user_config.h. Anything you leave out keeps its current value
(time zone UTC0 on the first run). A neighbourhood is precise enough, two decimals are about 1 km.

What it works out by itself around the position:
  - the name shown in the title bar: the nearest city (else town or village) from OpenStreetMap,
    in ASCII capitals (--name overrides it),
  - the coastline, so land and sea can be filled (an inland position is all land),
  - the nearest airport with an IATA code and an airliner-sized runway, shown on the radar as a reference mark and used for the
    arrival / departure colours and the approach lines of the ATC theme (--airport picks another one),
  - that airport's runways and the border of the region (OSM admin level 4) you are in.

The land outlines (kind 3) are closed rings made from the coastline: OSM keeps land on the left of
every coastline way, so a run that leaves the keep circle is joined to the next run that enters it,
going counter-clockwise along the circle. The firmware fills them (even-odd) for the themes that
colour land and sea.

Data is OpenStreetMap (c) OpenStreetMap contributors, ODbL, fetched from the public Overpass API and
cached in tools/.cache, so re-running for the same place is offline-safe. Positions beyond 80 degrees
of latitude or next to the antimeridian (longitude +-180) are not supported.
"""
import argparse
import bisect
import hashlib
import json
import math
import os
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request

KEEP_KM = 125.0      # keep geometry this far from the centre (the radar draws 100 km)
TOLERANCE_KM = 0.07  # Douglas-Peucker tolerance, well under one pixel at 25 km range
UNIT_KM = 0.05       # coordinates are stored as int16 multiples of this (see mapdata.h)
MAX_POINTS = 65535   # MapLine::first is a uint16
KM_PER_DEG = 111.195
AIRPORT_SEARCH_KM = 100
RUNWAY_RADIUS_M = 6000
OVERPASS = ["https://overpass-api.de/api/interpreter", "https://overpass.kumi.systems/api/interpreter",
            "https://overpass.private.coffee/api/interpreter"]  # tried in order, the main one is often overloaded
USER_AGENT = "CrowPanel-FlightScanner/1.0 (hobby ADS-B radar map generator)"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---- user_config.h ---------------------------------------------------------------------------

def read_user_config(path):
    """The values currently in user_config.h (an empty dict when there is none)."""
    if not os.path.exists(path):
        return {}
    text = open(path, encoding="utf-8").read()

    def find(pattern):
        m = re.search(pattern, text)
        return m.group(1) if m else None

    c = {}
    for key, pat in (("lat", r"HOME_LAT\s*=\s*(-?[0-9.]+)f"), ("lon", r"HOME_LON\s*=\s*(-?[0-9.]+)f"),
                     ("ref_lat", r"REF_AIRPORT_LAT\s+(-?[0-9.]+)f"), ("ref_lon", r"REF_AIRPORT_LON\s+(-?[0-9.]+)f")):
        v = find(pat)
        if v is not None:
            c[key] = float(v)
    for key, pat in (("name", r'#define\s+HOME_NAME\s+"([^"]*)"'), ("tz", r'#define\s+TZ_INFO\s+"([^"]*)"'),
                     ("ref_code", r'#define\s+REF_AIRPORT_CODE\s+"([^"]*)"')):
        v = find(pat)
        if v is not None:
            c[key] = v
    v = find(r"#define\s+SHOW_HOME_COORDS\s+([01])")
    if v is not None:
        c["show_coords"] = v == "1"
    return c


def write_user_config(path, c):
    lines = [
        "// Where the radar is centred, your time zone and the reference airport.",
        "// Written by tools/set_location.py, and you can edit it by hand too. This file is git-ignored on",
        "// purpose: it holds your position, so keep it out of anything you publish.",
        "#pragma once",
        "",
        "constexpr float HOME_LAT = %.6ff;" % c["lat"],
        "constexpr float HOME_LON = %.6ff;" % c["lon"],
        '#define HOME_NAME "%s"  // shown in the title bar, ASCII only' % c["name"],
        "#define SHOW_HOME_COORDS %d  // 1 = also print the coordinates in the title bar" % (1 if c["show_coords"] else 0),
        "",
    ]
    if c.get("ref_code"):
        lines += [
            "// Reference airport: drawn on the radar, and used for the arrival / departure colours and the",
            "// approach lines of the ATC theme. Remove these three lines if you do not want one.",
            '#define REF_AIRPORT_CODE "%s"' % c["ref_code"],
            "#define REF_AIRPORT_LAT %.6ff" % c["ref_lat"],
            "#define REF_AIRPORT_LON %.6ff" % c["ref_lon"],
        ]
    else:
        lines += ["// No reference airport. To add one, define REF_AIRPORT_CODE, REF_AIRPORT_LAT and REF_AIRPORT_LON."]
    lines += [
        "",
        "// POSIX time zone string for the clock, for example:",
        '//   "UTC0"   "<+03>-3"   "CET-1CEST,M3.5.0,M10.5.0/3"   "GMT0BST,M3.5.0/1,M10.5.0"   "EST5EDT,M3.2.0,M11.1.0"',
        '#define TZ_INFO "%s"' % c["tz"],
        "",
    ]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


NAME_MAX = 14  # characters that fit the title bar in every theme

_ASCII_FIXES = str.maketrans({"ı": "i", "ß": "ss", "ø": "o", "æ": "ae", "œ": "oe", "ł": "l", "đ": "d", "ð": "d",
                              "þ": "th", "ħ": "h", "Ø": "O", "Æ": "AE", "Œ": "OE", "Ł": "L", "Đ": "D", "Ð": "D",
                              "Þ": "TH", "Ħ": "H"})


def ascii_name(s, strict=False):
    """Upper-case ASCII for the title bar font: accents are dropped (Zürich -> ZURICH, İzmir -> IZMIR).

    With strict, a name that has letters of another script (Cyrillic, Greek, CJK ...) gives None
    instead of losing them.
    """
    out = []
    for ch in unicodedata.normalize("NFKD", s.translate(_ASCII_FIXES)):
        if unicodedata.combining(ch):
            continue
        if ch.isascii():
            out.append(ch)
        elif strict and ch.isalpha():
            return None
    s = re.sub(r"[^A-Za-z0-9 .\-]", "", "".join(out))
    s = re.sub(r" +", " ", s).strip().upper()
    return s or None


def fit_name(s):
    """Cut at a word boundary when the name is longer than the title bar allows."""
    if len(s) <= NAME_MAX:
        return s
    out = s.split(" ")[0]
    for word in s.split(" ")[1:]:
        if len(out) + 1 + len(word) > NAME_MAX:
            break
        out += " " + word
    return out[:NAME_MAX].strip()


def clean_name(s):
    """The name as the title bar shows it, "HOME" when nothing usable is left."""
    s = ascii_name(s)
    return fit_name(s) if s else "HOME"


# ---- Overpass ----------------------------------------------------------------------------------

def overpass(label, query, cache, soft=False):
    """Runs an Overpass query, or returns the cached answer of the same query.

    When no server answers it exits, or with soft (for things that are nice to have) tries once and
    returns None.
    """
    key = hashlib.sha1(query.encode()).hexdigest()[:10]
    path = os.path.join(cache, "%s-%s.json" % (label, key))
    if not os.path.exists(path):
        body = None
        for attempt in range(1 if soft else 2):
            for url in OVERPASS:
                print("fetching %s from %s ..." % (label, url), flush=True)
                req = urllib.request.Request(
                    url,
                    data=urllib.parse.urlencode({"data": query}).encode(),
                    headers={"User-Agent": USER_AGENT, "Accept": "*/*"},
                )
                try:
                    with urllib.request.urlopen(req, timeout=300) as r:
                        body = r.read()
                    remark = json.loads(body).get("remark", "")
                    if "runtime error" in remark:  # Overpass answers 200 with a partial result on timeouts
                        raise ValueError(remark)
                except (OSError, ValueError) as e:  # HTTPError and URLError are both OSErrors
                    print("  failed:", e, flush=True)
                    body = None
                    continue
                break
            if body is not None:
                break
            if attempt == 0 and not soft:
                print("  no server answered, retrying in 10 s ...", flush=True)
                time.sleep(10)
        if body is None:
            if soft:
                return None
            sys.exit("no Overpass server answered for %s, try again in a few minutes" % label)
        tmp = path + ".part"
        open(tmp, "wb").write(body)
        os.replace(tmp, path)
    return json.load(open(path, encoding="utf-8"))


def haversine_km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2
    return 2 * 6371.0 * math.asin(math.sqrt(a))


def runway_length_km(geometry):
    return sum(haversine_km(a["lat"], a["lon"], b["lat"], b["lon"]) for a, b in zip(geometry, geometry[1:]))


PLACE_TIERS = (("city", 30.0), ("town", 15.0), ("village", 5.0))  # OSM place kind, how far away it still counts (km)


def place_label(tags):
    """The title-bar spelling of a place: its own name when that survives the ASCII conversion, else English."""
    for key in ("name", "name:en", "int_name", "name:latin"):
        if tags.get(key):
            s = ascii_name(tags[key], strict=True)
            if s:
                return fit_name(s)
    return None


def find_place_name(lat, lon, cache):
    """(label, kind, km) of the place the position belongs to, or None.

    The nearest city within 30 km, else the nearest town within 15 km, else the nearest village within 5 km
    (OpenStreetMap place nodes), so a spot in a suburb is named after its city.
    """
    query = "[out:json][timeout:25];(%s);out;" % "".join(
        'node["place"="%s"]["name"](around:%d,%.5f,%.5f);' % (kind, km * 1000, lat, lon) for kind, km in PLACE_TIERS)
    data = overpass("place", query, cache, soft=True)
    if data is None:
        return None
    for kind, km in PLACE_TIERS:
        best = None
        for e in data.get("elements", []):
            tags = e.get("tags", {})
            if tags.get("place") != kind:
                continue
            d = haversine_km(lat, lon, e["lat"], e["lon"])
            label = place_label(tags)
            if label and d <= km and (best is None or d < best[2]):
                best = (label, kind, d)
        if best:
            return best
    return None


def find_airport(lat, lon, code, cache):
    """The nearest airport that really has an airliner-sized runway, or the one named by `code`.

    Candidates need an IATA code and a runway of at least 1 km at the airport, which leaves out
    seaplane bases and heliports. Military and private fields are skipped. Where some candidates are
    tagged international or public only those count. None when nothing qualifies.
    """
    if code:
        query = '[out:json][timeout:60];nwr["aeroway"="aerodrome"]["iata"="%s"];out center tags;' % code
    else:
        query = ('[out:json][timeout:60];nwr["aeroway"="aerodrome"]["iata"](around:%d,%.5f,%.5f);out center tags;'
                 % (AIRPORT_SEARCH_KM * 1000, lat, lon))
    found = []
    for e in overpass("airport", query, cache)["elements"]:
        tags = e.get("tags", {})
        c = e.get("center") or e
        iata = tags.get("iata", "")
        if not re.fullmatch(r"[A-Z]{3}", iata) or "lat" not in c:
            continue
        if not code and (tags.get("military") or tags.get("aerodrome:type") == "military"
                         or tags.get("access") in ("private", "no") or tags.get("seaplane") == "yes"):
            continue
        found.append([haversine_km(lat, lon, c["lat"], c["lon"]), iata, c["lat"], c["lon"], tags.get("name", ""),
                      tags.get("aerodrome") == "international" or tags.get("aerodrome:type") in ("international", "public")])
    if found and not code:
        runways = overpass("runways", '[out:json][timeout:90];way["aeroway"="runway"](around:%d,%.5f,%.5f);out geom;'
                           % (AIRPORT_SEARCH_KM * 1000 + 5000, lat, lon), cache)
        long_runways = [w["geometry"][0] for w in runways["elements"]
                        if w.get("geometry") and len(w["geometry"]) >= 2 and runway_length_km(w["geometry"]) >= 1.0]
        found = [f for f in found if any(haversine_km(f[2], f[3], p["lat"], p["lon"]) < 4.0 for p in long_runways)]
        if any(f[5] for f in found):
            found = [f for f in found if f[5]]
    found.sort()
    if found and not code:
        print("airports near you: " + "; ".join("%s %.0f km" % (f[1], f[0]) for f in found[:4]) +
              "  (--airport CODE picks another)")
    return found[0] if found else None


# ---- map geometry ------------------------------------------------------------------------------

def ways_of(doc):
    out = []
    for e in doc["elements"]:
        if e["type"] == "way" and e.get("geometry"):
            out.append([(p["lat"], p["lon"]) for p in e["geometry"]])
        elif e["type"] == "relation":
            for m in e.get("members", []):
                if m["type"] == "way" and m.get("geometry") and m.get("role") in ("outer", ""):
                    out.append([(p["lat"], p["lon"]) for p in m["geometry"]])
    return out


def stitch(ways):
    """Joins ways that share an end point (either direction) into longer polylines."""
    ends = {}
    for i, w in enumerate(ways):
        ends.setdefault(w[0], []).append(i)
        ends.setdefault(w[-1], []).append(i)
    used = [False] * len(ways)
    lines = []
    for i in range(len(ways)):
        if used[i]:
            continue
        used[i] = True
        line = list(ways[i])
        for at_tail in (True, False):  # extend the tail first, then the head
            while True:
                p = line[-1] if at_tail else line[0]
                nxt = next((j for j in ends.get(p, []) if not used[j]), None)
                if nxt is None:
                    break
                used[nxt] = True
                w = ways[nxt]
                if at_tail:
                    if w[0] != p:
                        w = w[::-1]
                    line.extend(w[1:])
                else:
                    if w[-1] != p:
                        w = w[::-1]
                    line[:0] = w[:-1]
        lines.append(line)
    return lines


def keep_runs(pts):
    """Splits a polyline into runs near the centre, keeping one point beyond each edge so lines still cross the radar rim."""
    near = [math.hypot(*p) <= KEEP_KM for p in pts]
    runs, cur = [], []
    for i, p in enumerate(pts):
        keep = near[i] or (i > 0 and near[i - 1]) or (i + 1 < len(pts) and near[i + 1])
        if keep:
            cur.append(p)
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return [r for r in runs if len(r) >= 2]


def simplify(pts, tol):
    """Iterative Douglas-Peucker."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        far, far_d = -1, tol
        for i in range(a + 1, b):
            px, py = pts[i]
            d = math.hypot(px - ax, py - ay) if norm == 0 else abs(dx * (ay - py) - (ax - px) * dy) / norm
            if d > far_d:
                far, far_d = i, d
        if far >= 0:
            keep[far] = True
            stack += [(a, far), (far, b)]
    return [p for p, k in zip(pts, keep) if k]


def circle_crossing(outside, inside, r):
    """Where the segment outside -> inside first crosses the circle of radius r about the centre."""
    dx, dy = inside[0] - outside[0], inside[1] - outside[1]
    a = dx * dx + dy * dy
    b = 2 * (outside[0] * dx + outside[1] * dy)
    c = outside[0] ** 2 + outside[1] ** 2 - r * r
    t = (-b - math.sqrt(max(b * b - 4 * a * c, 0.0))) / (2 * a)
    t = min(max(t, 0.0), 1.0)
    return (outside[0] + t * dx, outside[1] + t * dy)


def trim_to_circle(run):
    """Moves the one-beyond end points of a run onto the keep circle, so open runs start and end on it."""
    run = list(run)
    if math.hypot(*run[0]) > KEEP_KM:
        run[0] = circle_crossing(run[0], run[1], KEEP_KM)
    if math.hypot(*run[-1]) > KEEP_KM:
        run[-1] = circle_crossing(run[-1], run[-2], KEEP_KM)
    return run


def build(doc, kind, to_km, tol):
    result = []
    for line in stitch(ways_of(doc)):
        km = [to_km(lat, lon) for lat, lon in line]
        if kind == 0 and len(km) > 3 and km[0] == km[-1]:
            # A closed loop that is partly outside must not be split in its inside part: start it outside.
            out = next((i for i, p in enumerate(km) if math.hypot(*p) > KEEP_KM), None)
            if out is not None:
                km = km[out:-1] + km[:out] + [km[out]]
        for run in keep_runs(km):
            if kind == 0:
                run = trim_to_circle(run)
            s = simplify(run, tol)
            if len(s) >= 2:
                result.append((kind, s))
    return result


def make_rings(runs):
    """Closed land outlines from coastline runs (land is on the left of every run)."""
    rings = [list(s) for s in runs if s[0] == s[-1]]
    open_runs = [s for s in runs if s[0] != s[-1]]
    two_pi = 2 * math.pi

    def ang(p):
        return math.atan2(p[1], p[0]) % two_pi

    for s in open_runs:
        for p in (s[0], s[-1]):
            if abs(math.hypot(*p) - KEEP_KM) > 0.01:
                print("warning: open coast run ends inside the keep circle at %.1f,%.1f km" % (p[0], p[1]))
    entries = sorted(range(len(open_runs)), key=lambda i: ang(open_runs[i][0]))
    eang = [ang(open_runs[i][0]) for i in entries]
    used = [False] * len(open_runs)
    for start in range(len(open_runs)):
        if used[start]:
            continue
        ring, i = [], start
        while True:
            used[i] = True
            ring.extend(open_runs[i])
            a0 = ang(open_runs[i][-1])
            k = bisect.bisect_left(eang, a0 - 1e-9) % len(entries)
            j, a1 = entries[k], eang[k]
            delta = (a1 - a0) % two_pi
            n = max(1, int(delta / math.radians(4)))
            for q in range(1, n):
                a = a0 + delta * q / n
                ring.append((KEEP_KM * math.cos(a), KEEP_KM * math.sin(a)))
            if j == start:
                ring.append(open_runs[start][0])
                break
            if used[j]:
                print("warning: coast runs do not pair up, land fill may be wrong")
                break
            i = j
        rings.append(ring)
    return rings


def inside(rings, x, y):
    """Even-odd point-in-polygon over all rings."""
    hit = False
    for ring in rings:
        for (x0, y0), (x1, y1) in zip(ring, ring[1:]):
            if (y0 <= y) != (y1 <= y) and x0 + (y - y0) * (x1 - x0) / (y1 - y0) > x:
                hit = not hit
    return hit


def disc_ring():
    """One land ring covering the whole keep disc (counter-clockwise, closed)."""
    n = 72
    ring = [(KEEP_KM * math.cos(2 * math.pi * i / n), KEEP_KM * math.sin(2 * math.pi * i / n)) for i in range(n)]
    return ring + [ring[0]]


# ---- main --------------------------------------------------------------------------------------

def main():
    for stream in (sys.stdout, sys.stderr):  # airport names may not fit the console's code page
        try:
            stream.reconfigure(errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(
        description="Sets the radar position and builds the map around it (see the docstring of this file).")
    ap.add_argument("--lat", type=float, help="latitude in decimal degrees, south is negative")
    ap.add_argument("--lon", type=float, help="longitude in decimal degrees, west is negative")
    ap.add_argument("--name", help="label for the title bar, ASCII (default: the name of the nearest city, found in OpenStreetMap)")
    ap.add_argument("--tz", help='POSIX time zone string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" (default UTC0)')
    ap.add_argument("--airport", metavar="IATA|none", help="reference airport instead of the nearest one, or none")
    ap.add_argument("--show-coords", dest="show_coords", action="store_true", default=None,
                    help="print the coordinates in the title bar")
    ap.add_argument("--hide-coords", dest="show_coords", action="store_false", help="do not print them (default)")
    ap.add_argument("--ocean", action="store_true",
                    help="no coastline within 125 km and you are over open water: draw no land fill")
    ap.add_argument("--no-border", action="store_true", help="skip the region border (big regions download a lot)")
    ap.add_argument("--cache", default=os.path.join(ROOT, "tools", ".cache"), help="download cache folder")
    ap.add_argument("--out", default=os.path.join(ROOT, "include"), help="where user_config.h and map_local.inc go")
    args = ap.parse_args()

    if (args.lat is None) != (args.lon is None):
        sys.exit("give --lat and --lon together")
    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    cfg_path = os.path.join(args.out, "user_config.h")
    c = read_user_config(cfg_path)
    moved = args.lat is not None
    if moved:
        c["lat"], c["lon"] = args.lat, args.lon
    if "lat" not in c or "lon" not in c or (c["lat"] == 0 and c["lon"] == 0):
        sys.exit("no position yet: run  python -I tools/set_location.py --lat <degrees> --lon <degrees>\n"
                 "(see the README, or --help)")
    if abs(c["lat"]) > 80 or abs(c["lon"]) > 179:
        sys.exit("positions beyond 80 degrees of latitude or next to longitude +-180 are not supported")
    if args.name:
        c["name"] = clean_name(args.name)
    elif moved or "name" not in c:
        found = find_place_name(c["lat"], c["lon"], args.cache)
        if found:
            c["name"] = found[0]
            print("place name: %s (nearest %s, %.1f km away; --name sets another)" % (found[0], found[1], found[2]))
        else:
            c["name"] = "HOME"
            print("no place name found (OpenStreetMap did not answer, or nothing is near): using HOME, "
                  "--name sets one")
    c["tz"] = args.tz if args.tz else c.get("tz", "UTC0")
    if not re.fullmatch(r"[A-Za-z0-9<>+\-:,./]+", c["tz"]):
        sys.exit("odd characters in the time zone string")
    c["show_coords"] = args.show_coords if args.show_coords is not None else c.get("show_coords", False)
    if not args.tz and "tz" not in read_user_config(cfg_path):
        print("note: time zone is UTC0, pass --tz to show your local time (examples in the README)")

    lat0, lon0 = c["lat"], c["lon"]
    cos_lat = math.cos(math.radians(lat0))

    def to_km(lat, lon):
        return (lon - lon0) * KM_PER_DEG * cos_lat, (lat - lat0) * KM_PER_DEG

    # Reference airport: nearest by default, kept as it is when only the map is rebuilt.
    if args.airport and args.airport.lower() == "none":
        for k in ("ref_code", "ref_lat", "ref_lon"):
            c.pop(k, None)
    elif args.airport or moved or not c.get("ref_code"):
        code = args.airport.upper() if args.airport else None
        if code and not re.fullmatch(r"[A-Z]{3}", code):
            sys.exit("--airport takes a 3 letter IATA code or none")
        apt = find_airport(lat0, lon0, code, args.cache)
        if apt is None:
            print("no airport with an IATA code and a 1 km runway within %d km: no reference airport" % AIRPORT_SEARCH_KM
                  if not code else "airport %s not found" % code)
            if code:
                sys.exit(1)
            for k in ("ref_code", "ref_lat", "ref_lon"):
                c.pop(k, None)
        else:
            c["ref_code"], c["ref_lat"], c["ref_lon"] = apt[1], apt[2], apt[3]
            print("reference airport: %s %s, %.1f km away" % (apt[1], apt[4], apt[0]))

    dlat = KEEP_KM / KM_PER_DEG
    dlon = KEEP_KM / (KM_PER_DEG * cos_lat)
    bbox = "%.4f,%.4f,%.4f,%.4f" % (lat0 - dlat, lon0 - dlon, lat0 + dlat, lon0 + dlon)
    coast = overpass("coast", '[out:json][timeout:120];way["natural"="coastline"](%s);out geom;' % bbox, args.cache)
    if args.no_border:
        border = {"elements": []}
    else:
        border = overpass("border", '[out:json][timeout:120];is_in(%.5f,%.5f)->.a;'
                          'rel(pivot.a)["boundary"="administrative"]["admin_level"="4"];out geom;' % (lat0, lon0),
                          args.cache)
    runway = {"elements": []}
    if c.get("ref_code"):
        runway = overpass("runway", '[out:json][timeout:60];way["aeroway"="runway"](around:%d,%.5f,%.5f);out geom;'
                          % (RUNWAY_RADIUS_M, c["ref_lat"], c["ref_lon"]), args.cache)

    runway_lines = []
    for w in ways_of(runway):
        km = [to_km(lat, lon) for lat, lon in w]
        if len(km) >= 2 and math.hypot(*km[0]) <= KEEP_KM:
            runway_lines.append((2, km))

    tol = TOLERANCE_KM
    while True:
        coast_lines = build(coast, 0, to_km, tol)
        if coast_lines:
            rings = make_rings([s for _, s in coast_lines])
            area = sum(sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(r, r[1:])) / 2 for r in rings)
            print("%d land rings, net area %.0f km2 (disc %.0f)" % (len(rings), area, math.pi * KEEP_KM ** 2))
            if area <= 0 or area >= math.pi * KEEP_KM ** 2:
                sys.exit("land area is not between 0 and the keep disc: ring joining is wrong")
            if not inside(rings, 0.0, 0.0):
                print("note: the centre is not on land (open water, or the coast data is odd)")
        elif args.ocean:
            rings = []
            print("no coastline within %.0f km: no land fill (--ocean)" % KEEP_KM)
        else:
            rings = [disc_ring()]
            print("no coastline within %.0f km: the whole radar is land (use --ocean if you are over open water)"
                  % KEEP_KM)
        lines = coast_lines + build(border, 1, to_km, tol) + runway_lines + [(3, r) for r in rings]
        points = sum(len(s) for _, s in lines)
        if points <= MAX_POINTS:
            break
        tol *= 1.5
        print("%d points are too many, retrying with a coarser tolerance (%.2f km)" % (points, tol))

    out = [
        "// Generated by tools/set_location.py from OpenStreetMap data (c) OpenStreetMap contributors, ODbL.",
        "// Coastline, region border, runways and land outlines around your position, flat-earth km / MAP_UNIT_KM.",
        "// Do not edit. Git-ignored on purpose: it shows where you are.",
        "const int16_t MAP_PTS[] = {",
    ]
    first, idx, row = [], 0, []
    for kind, pts in lines:
        first.append((idx, len(pts), kind))
        for x, y in pts:
            row.append("%d,%d" % (round(x / UNIT_KM), round(y / UNIT_KM)))
            if len(row) == 12:
                out.append("    " + ",".join(row) + ",")
                row = []
        idx += len(pts)
    if row:
        out.append("    " + ",".join(row) + ",")
    if not first:  # C++ has no empty arrays
        out.append("    0,0,")
        first_rows = ["    {0, 0, 0},"]
    else:
        first_rows = ["    {%d, %d, %d}," % f for f in first]
    out += ["};", "", "const MapLine MAP_LINES[] = {"] + first_rows + ["};",
            "const int MAP_LINE_COUNT = %d;" % len(first), ""]

    map_path = os.path.join(args.out, "map_local.inc")
    open(map_path, "w", encoding="utf-8", newline="\n").write("\n".join(out))
    write_user_config(cfg_path, c)
    by_kind = {0: 0, 1: 0, 2: 0, 3: 0}
    for k, s in lines:
        by_kind[k] += len(s)
    print("%d polylines, %d points (coast %d, border %d, runway %d, land %d), %d KB" % (
        len(lines), points, by_kind[0], by_kind[1], by_kind[2], by_kind[3], points * 4 // 1024))
    print("wrote %s and %s (both are git-ignored)" % (cfg_path, map_path))


if __name__ == "__main__":
    main()
