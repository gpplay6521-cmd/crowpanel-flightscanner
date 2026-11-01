#include "route.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

// Flights are identified by callsign. adsbdb.com answers with airline, airports and their
// coordinates; adsb.lol's vrs-standing-data (static files, one per callsign) is the fallback
// when adsbdb has never heard of the callsign. Both describe the *scheduled* route of that
// callsign, not what the aircraft is cleared to fly right now.

namespace {

constexpr int CAP = 40;                     // callsigns remembered
constexpr uint32_t FOUND_TTL_MS = 3UL * 3600 * 1000;
constexpr uint32_t MISS_TTL_MS = 15UL * 60 * 1000;
constexpr uint32_t PACE_MS = 800;           // gap between lookups: keeps core 0 and the APIs calm
constexpr uint8_t MAX_FAILS = 3;

struct Entry {
  char call[10];
  RouteInfo info;
  uint32_t usedMs;     // last routeLookup()
  uint32_t doneMs;     // last time `info` was resolved
  uint32_t retryAtMs;  // earliest next attempt after a transport error
  uint8_t fails;
  bool used;
  bool queued;    // wants a (re)lookup
  bool urgent;    // the selected aircraft: goes first
  bool inflight;  // the network task is working on it right now
};

SemaphoreHandle_t lock;
Entry cache[CAP];
uint32_t lastLookupMs = 0;

// "THY3385", "sxs9qj " -> uppercase, 3..8 alphanumerics, starts with two letters and has a digit.
// That is what airline callsigns look like; registrations ("TCJRB", "N123AB") never resolve.
bool normalizeCallsign(char* dst, const char* src) {
  while (*src == ' ') ++src;
  size_t n = 0;
  bool digit = false;
  for (; *src && *src != ' '; ++src) {
    char c = *src;
    if (c >= 'a' && c <= 'z') c -= 32;
    bool alpha = c >= 'A' && c <= 'Z', num = c >= '0' && c <= '9';
    if (!alpha && !num) return false;
    if (n >= 8) return false;
    if (n < 2 && !alpha) return false;
    digit |= num;
    dst[n++] = c;
  }
  dst[n] = 0;
  return n >= 3 && digit;
}

// The GFX fonts only have ASCII glyphs; reduce UTF-8 names ("İzmir", "Münih") to plain letters.
char foldCodePoint(uint32_t cp) {
  static const char LATIN1[] =
      "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"   // U+00C0..DF
      "aaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00E0..FF
  if (cp >= 0xC0 && cp <= 0xFF) return LATIN1[cp - 0xC0];
  switch (cp) {
    case 0x0104: return 'A';
    case 0x0105: return 'a';
    case 0x0106: case 0x010C: return 'C';
    case 0x0107: case 0x010D: return 'c';
    case 0x011A: return 'E';
    case 0x011B: return 'e';
    case 0x0118: return 'E';
    case 0x0119: return 'e';
    case 0x011E: return 'G';
    case 0x011F: return 'g';
    case 0x0130: return 'I';
    case 0x0131: return 'i';
    case 0x0141: return 'L';
    case 0x0142: return 'l';
    case 0x0143: case 0x0147: return 'N';
    case 0x0144: case 0x0148: return 'n';
    case 0x0150: return 'O';
    case 0x0151: return 'o';
    case 0x0158: return 'R';
    case 0x0159: return 'r';
    case 0x015A: case 0x015E: case 0x0160: return 'S';
    case 0x015B: case 0x015F: case 0x0161: return 's';
    case 0x0170: return 'U';
    case 0x0171: return 'u';
    case 0x0179: case 0x017B: case 0x017D: return 'Z';
    case 0x017A: case 0x017C: case 0x017E: return 'z';
    case 0x2013: case 0x2014: return '-';
    case 0x2019: return '\'';
    default: return 0;
  }
}

void fold(char* dst, size_t cap, const char* src) {
  size_t n = 0;
  while (*src && n + 1 < cap) {
    uint8_t c = (uint8_t)*src;
    uint32_t cp;
    int extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { ++src; continue; }
    ++src;
    for (; extra > 0 && ((uint8_t)*src & 0xC0) == 0x80; --extra, ++src) cp = (cp << 6) | ((uint8_t)*src & 0x3F);
    char out = cp < 0x80 ? (char)cp : foldCodePoint(cp);
    if (out >= 0x20) dst[n++] = out;
  }
  dst[n] = 0;
  while (n > 0 && dst[n - 1] == ' ') dst[--n] = 0;
}

struct Country {
  char iso[3];
  const char* name;
};
const Country COUNTRIES[] = {
    {"AE", "UAE"}, {"AL", "Albania"}, {"AM", "Armenia"}, {"AT", "Austria"}, {"AZ", "Azerbaijan"},
    {"BA", "Bosnia & Herz."}, {"BE", "Belgium"}, {"BG", "Bulgaria"}, {"BH", "Bahrain"}, {"BY", "Belarus"},
    {"CH", "Switzerland"}, {"CN", "China"}, {"CY", "Cyprus"}, {"CZ", "Czechia"}, {"DE", "Germany"},
    {"DK", "Denmark"}, {"DZ", "Algeria"}, {"EE", "Estonia"}, {"EG", "Egypt"}, {"ES", "Spain"},
    {"FI", "Finland"}, {"FR", "France"}, {"GB", "United Kingdom"}, {"GE", "Georgia"}, {"GR", "Greece"},
    {"HR", "Croatia"}, {"HU", "Hungary"}, {"IE", "Ireland"}, {"IL", "Israel"}, {"IN", "India"},
    {"IQ", "Iraq"}, {"IR", "Iran"}, {"IS", "Iceland"}, {"IT", "Italy"}, {"JO", "Jordan"},
    {"KW", "Kuwait"}, {"KZ", "Kazakhstan"}, {"LB", "Lebanon"}, {"LT", "Lithuania"}, {"LU", "Luxembourg"},
    {"LV", "Latvia"}, {"LY", "Libya"}, {"MA", "Morocco"}, {"MD", "Moldova"}, {"ME", "Montenegro"},
    {"MK", "North Macedonia"}, {"MT", "Malta"}, {"NL", "Netherlands"}, {"NO", "Norway"}, {"OM", "Oman"},
    {"PL", "Poland"}, {"PT", "Portugal"}, {"QA", "Qatar"}, {"RO", "Romania"}, {"RS", "Serbia"},
    {"RU", "Russia"}, {"SA", "Saudi Arabia"}, {"SE", "Sweden"}, {"SI", "Slovenia"}, {"SK", "Slovakia"},
    {"SY", "Syria"}, {"TH", "Thailand"}, {"TN", "Tunisia"}, {"TR", "Turkey"}, {"UA", "Ukraine"},
    {"US", "United States"}, {"UZ", "Uzbekistan"},
};

// ISO 3166 alpha-2 -> short name; countries not in the table keep the name the API sent (or the code).
void setCountry(char* dst, size_t cap, const char* iso, const char* apiName) {
  for (const Country& c : COUNTRIES)
    if (strcasecmp(c.iso, iso) == 0) {
      strlcpy(dst, c.name, cap);
      return;
    }
  fold(dst, cap, apiName[0] ? apiName : iso);
}

constexpr int NAME_WORDS = 10, NAME_WORD_LEN = 24, SHORT_NAME_MAX = 17;

// Splits into words; `dropGeneric` leaves out "International", "Airport" and friends.
int splitWords(const char* s, char (*out)[NAME_WORD_LEN], bool dropGeneric) {
  static const char* const GENERIC[] = {"International", "Intl", "Airport",    "Aerodrome", "Airfield",
                                        "Aeropuerto",    "Aeroporto", "Aeroport", "Flughafen", "Havalimani"};
  char buf[64];
  strlcpy(buf, s, sizeof buf);
  int n = 0;
  for (char* w = strtok(buf, " "); w && n < NAME_WORDS; w = strtok(nullptr, " ")) {
    bool generic = false;
    for (const char* g : GENERIC) generic = generic || strcasecmp(w, g) == 0;
    if (!generic || !dropGeneric) strlcpy(out[n++], w, NAME_WORD_LEN);
  }
  return n;
}

// "London Gatwick Airport" in London ->
// "Gatwick"; "Larnaca International Airport" in Larnaca -> "Larnaca Airport". At most SHORT_NAME_MAX characters.
void shortName(char* dst, size_t cap, const char* full, const char* city) {
  char words[NAME_WORDS][NAME_WORD_LEN], cityWords[NAME_WORDS][NAME_WORD_LEN];
  int n = splitWords(full, words, true);
  int cn = splitWords(city, cityWords, false);
  int skip = 0;
  while (skip < cn && skip < n - 1 && strcasecmp(words[skip], cityWords[skip]) == 0) ++skip;  // keep >= 1 word

  char out[64] = "";
  for (int i = skip; i < n; ++i) {
    if (i > skip) strlcat(out, " ", sizeof out);
    strlcat(out, words[i], sizeof out);
  }
  if (!out[0]) strlcpy(out, "Airport", sizeof out);
  else if (strcasecmp(out, city) == 0) {  // same as the city line: add what it is, as short as fits
    if (strlen(out) + 8 <= SHORT_NAME_MAX) strlcat(out, " Airport", sizeof out);
    else if (strlen(out) + 4 <= SHORT_NAME_MAX) strlcat(out, " Apt", sizeof out);
  }

  while (strlen(out) > SHORT_NAME_MAX) {  // drop leading words, then cut
    char* sp = strchr(out, ' ');
    if (!sp) break;
    memmove(out, sp + 1, strlen(sp + 1) + 1);
  }
  out[SHORT_NAME_MAX] = 0;
  strlcpy(dst, out, cap);
}

void fillAirport(Airport& a, JsonObjectConst o, const char* iataKey, const char* icaoKey, const char* cityKey,
                 const char* latKey, const char* lonKey, const char* isoKey, const char* countryKey) {
  a = {};
  fold(a.iata, sizeof a.iata, o[iataKey] | "");
  fold(a.icao, sizeof a.icao, o[icaoKey] | "");
  fold(a.city, sizeof a.city, o[cityKey] | "");
  char full[64];
  fold(full, sizeof full, o["name"] | "");
  shortName(a.name, sizeof a.name, full, a.city);
  setCountry(a.country, sizeof a.country, o[isoKey] | "", o[countryKey] | "");
  a.lat = o[latKey] | 0.0f;
  a.lon = o[lonKey] | 0.0f;
}

bool airportUsable(const Airport& a) { return (a.iata[0] || a.icao[0]) && (a.lat != 0.0f || a.lon != 0.0f); }

bool parseAdsbdb(const String& body, RouteInfo& r) {
  JsonDocument filter;
  filter["response"]["flightroute"]["airline"]["name"] = true;
  for (const char* k : {"iata_code", "icao_code", "latitude", "longitude", "municipality", "name", "country_iso_name", "country_name"}) {
    filter["response"]["flightroute"]["origin"][k] = true;
    filter["response"]["flightroute"]["destination"][k] = true;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  JsonObjectConst fr = doc["response"]["flightroute"];
  if (fr.isNull()) return false;
  fold(r.airline, sizeof r.airline, fr["airline"]["name"] | "");
  fillAirport(r.from, fr["origin"], "iata_code", "icao_code", "municipality", "latitude", "longitude", "country_iso_name",
              "country_name");
  fillAirport(r.to, fr["destination"], "iata_code", "icao_code", "municipality", "latitude", "longitude", "country_iso_name",
              "country_name");
  return airportUsable(r.from) && airportUsable(r.to);
}

// A multi-stop callsign lists several airports; show first -> last.
bool parseVrs(const String& body, RouteInfo& r) {
  JsonDocument filter;
  JsonObject f = filter["_airports"].add<JsonObject>();
  for (const char* k : {"name", "icao", "iata", "location", "lat", "lon", "countryiso2"}) f[k] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  JsonArrayConst ap = doc["_airports"];
  if (ap.size() < 2) return false;
  r.airline[0] = 0;
  fillAirport(r.from, ap[0], "iata", "icao", "location", "lat", "lon", "countryiso2", "countryiso2");
  fillAirport(r.to, ap[ap.size() - 1], "iata", "icao", "location", "lat", "lon", "countryiso2", "countryiso2");
  return airportUsable(r.from) && airportUsable(r.to);
}

enum class Http { Ok, Missing, Error };

Http httpGet(const char* host, const char* path, String& body) {
  char url[128];
  snprintf(url, sizeof url, "https://%s%s", host, path);
  WiFiClientSecure client;
  client.setInsecure();  // public read-only data, same as the position feeds
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);
  http.setUserAgent("CrowPanel-FlightScanner/1.0");
  if (!http.begin(client, url)) return Http::Error;
  int code = http.GET();
  Http result;
  if (code == HTTP_CODE_OK) {
    body = http.getString();
    result = body.length() ? Http::Ok : Http::Error;
  } else if (code == HTTP_CODE_NOT_FOUND) {
    result = Http::Missing;  // vrs-standing-data answers with a big HTML page here: never read it
  } else {
    Serial.printf("[route] %s HTTP %d\n", host, code);
    result = Http::Error;
  }
  http.end();
  return result;
}

enum class Outcome { Found, Missing, Error };

Outcome resolve(const char* call, RouteInfo& r) {
  r = {};
  String body;
  char path[48];

  snprintf(path, sizeof path, "/v0/callsign/%s", call);
  Http a = httpGet("api.adsbdb.com", path, body);
  if (a == Http::Ok && parseAdsbdb(body, r)) {
    Serial.printf("[route] %s: %s -> %s (adsbdb)\n", call, r.from.iata, r.to.iata);
    return Outcome::Found;
  }

  snprintf(path, sizeof path, "/routes/%.2s/%s.json", call, call);
  Http b = httpGet("vrs-standing-data.adsb.lol", path, body);
  r = {};
  if (b == Http::Ok && parseVrs(body, r)) {
    Serial.printf("[route] %s: %s -> %s (vrs)\n", call, r.from.iata, r.to.iata);
    return Outcome::Found;
  }
  r = {};
  bool failed = a == Http::Error || b == Http::Error;
  Serial.printf("[route] %s: %s\n", call, failed ? "lookup failed" : "no route known");
  return failed ? Outcome::Error : Outcome::Missing;
}

Entry* find(const char* call) {
  for (Entry& e : cache)
    if (e.used && !strcmp(e.call, call)) return &e;
  return nullptr;
}

// Free slot, else the least recently shown settled entry, else the least recently shown queued one.
Entry* allocate(const char* call) {
  Entry* pick = nullptr;
  for (Entry& e : cache) {
    if (!e.used) { pick = &e; break; }
  }
  for (int pass = 0; !pick && pass < 2; ++pass) {
    for (Entry& e : cache) {
      if (e.inflight || (pass == 0 && e.queued)) continue;
      if (!pick || e.usedMs < pick->usedMs) pick = &e;
    }
  }
  if (!pick) return nullptr;
  *pick = {};
  strlcpy(pick->call, call, sizeof pick->call);
  pick->used = true;
  pick->info.state = RouteInfo::State::Loading;
  return pick;
}

}  // namespace

void routeBegin() { lock = xSemaphoreCreateMutex(); }

RouteInfo routeLookup(const char* callsign, bool urgent) {
  RouteInfo none = {};
  char call[10];
  if (!normalizeCallsign(call, callsign)) {
    none.state = RouteInfo::State::NotFound;
    return none;
  }
  none.state = RouteInfo::State::Loading;
  uint32_t now = millis();

  xSemaphoreTake(lock, portMAX_DELAY);
  Entry* e = find(call);
  if (!e) {
    e = allocate(call);
    if (e) e->queued = true;
  } else if (!e->queued && !e->inflight) {
    uint32_t ttl = e->info.state == RouteInfo::State::Found ? FOUND_TTL_MS : MISS_TTL_MS;
    if (now - e->doneMs > ttl) {
      e->queued = true;
      e->fails = 0;
    }
  }
  RouteInfo out = none;
  if (e) {
    e->usedMs = now;
    if (urgent && e->queued) e->urgent = true;
    out = e->info;
  }
  xSemaphoreGive(lock);
  return out;
}

bool routeWork() {
  uint32_t now = millis();
  if (now - lastLookupMs < PACE_MS) return false;

  char call[10];
  xSemaphoreTake(lock, portMAX_DELAY);
  Entry* job = nullptr;
  for (Entry& e : cache) {
    if (!e.used || !e.queued || e.inflight || (e.retryAtMs && (int32_t)(now - e.retryAtMs) < 0)) continue;
    bool better = !job || (e.urgent && !job->urgent) || (e.urgent == job->urgent && e.usedMs > job->usedMs);
    if (better) job = &e;
  }
  if (job) {
    job->inflight = true;
    strlcpy(call, job->call, sizeof call);
  }
  xSemaphoreGive(lock);
  if (!job) return false;

  RouteInfo r;
  Outcome o = resolve(call, r);
  lastLookupMs = millis();

  xSemaphoreTake(lock, portMAX_DELAY);
  job->inflight = false;
  if (o == Outcome::Error && ++job->fails < MAX_FAILS) {
    job->retryAtMs = millis() + 10000UL * job->fails;  // stay queued, try again later
  } else {
    job->queued = false;
    job->urgent = false;
    job->retryAtMs = 0;
    job->fails = 0;
    job->doneMs = millis();
    if (o == Outcome::Found) {
      job->info = r;
      job->info.state = RouteInfo::State::Found;
    } else if (job->info.state != RouteInfo::State::Found) {  // keep a stale route over a failed refresh
      job->info.state = RouteInfo::State::NotFound;
    }
  }
  xSemaphoreGive(lock);
  return true;
}
