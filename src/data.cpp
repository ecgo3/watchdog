#include "data.hpp"
#include "orbit.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <optional>
#include <tuple>
#include <cstdio>

namespace wd {
namespace {
using json = nlohmann::json;

enum class Payload { Json, Tle };
struct Response { std::string body; bool live{}; bool cached{}; };

std::size_t writeBytes(char* data, std::size_t size, std::size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

std::filesystem::path cacheRoot() {
    auto path = std::filesystem::temp_directory_path() / "watchdog-command-console-v2";
    std::error_code ec; std::filesystem::create_directories(path, ec);
    return path;
}

bool validPayload(const std::string& body, Payload kind) {
    const auto first = body.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) return false;
    if (kind == Payload::Json) return body[first] == '[' || body[first] == '{';
    return body.find("\n1 ") != std::string::npos && body.find("\n2 ") != std::string::npos;
}

Response cachedGet(const std::string& key, const std::string& url, Payload kind = Payload::Json,
                   std::chrono::minutes maxAge = std::chrono::minutes(15)) {
    Response out;
    const auto path = cacheRoot() / (key + (kind == Payload::Json ? ".json" : ".tle"));
    std::error_code ageError;
    if (std::filesystem::exists(path,ageError)) {
        const auto age=std::filesystem::file_time_type::clock::now()-std::filesystem::last_write_time(path,ageError);
        if (!ageError&&age<maxAge) {
            std::ifstream file(path,std::ios::binary);
            if (file) out.body.assign(std::istreambuf_iterator<char>(file),{});
            if (validPayload(out.body,kind)) { out.cached=true; return out; }
            out.body.clear();
        }
    }
    CURL* curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBytes);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 24L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 7L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "WATCHDOG/0.2 scientific-console; public-data-client");
        const auto result = curl_easy_perform(curl);
        long status = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.live = result == CURLE_OK && status >= 200 && status < 300 && validPayload(out.body, kind);
        curl_easy_cleanup(curl);
    }
    if (out.live) {
        std::ofstream file(path, std::ios::binary); file.write(out.body.data(), static_cast<std::streamsize>(out.body.size()));
    } else {
        std::ifstream file(path, std::ios::binary);
        if (file) out.body.assign(std::istreambuf_iterator<char>(file), {});
        out.cached = validPayload(out.body, kind);
        if (!out.cached) out.body.clear();
    }
    return out;
}

std::string urlEncode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) return value;
    char* encoded = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
    std::string out = encoded ? encoded : value;
    if (encoded) curl_free(encoded);
    curl_easy_cleanup(curl);
    return out;
}

std::string str(const json& o, const char* key, std::string fallback = "UNAVAILABLE") {
    if (!o.is_object() || !o.contains(key) || o[key].is_null()) return fallback;
    if (o[key].is_string()) return o[key].get<std::string>();
    if (o[key].is_boolean()) return o[key].get<bool>() ? "YES" : "NO";
    return o[key].dump();
}

double num(const json& o, const char* key, double fallback = 0.0) {
    if (!o.is_object() || !o.contains(key) || o[key].is_null()) return fallback;
    if (o[key].is_number()) return o[key].get<double>();
    try { return std::stod(o[key].get<std::string>()); } catch (...) { return fallback; }
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

bool containsCi(const std::string& value, const std::string& query) { return lower(value).find(lower(query)) != std::string::npos; }

std::string fixed(double value, int precision = 3) {
    std::ostringstream out; out << std::fixed << std::setprecision(precision) << value; return out.str();
}

std::string join(const std::vector<std::string>& values, const char* delimiter = " / ") {
    if (values.empty()) return "UNCATEGORIZED";
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << delimiter; out << values[i]; }
    return out.str();
}

ObjectKind typeFrom(const std::string& text) {
    const auto t = lower(text);
    if (t.find("asteroid") != std::string::npos) return ObjectKind::Asteroid;
    if (t.find("comet") != std::string::npos) return ObjectKind::Comet;
    if (t.find("spacecraft") != std::string::npos) return ObjectKind::Spacecraft;
    if (t.find("planet") != std::string::npos || t.find("sun") != std::string::npos) return ObjectKind::Planet;
    return ObjectKind::Unknown;
}

std::string opsStatus(const std::string& code) {
    if (code == "+") return "OPERATIONAL";
    if (code == "-") return "NONOPERATIONAL";
    if (code == "P") return "PARTIALLY OPERATIONAL";
    if (code == "B") return "BACKUP / STANDBY";
    if (code == "S") return "OPERATIONAL SPARE";
    if (code == "X") return "EXTENDED MISSION";
    if (code == "D") return "DECAYED";
    return "UNKNOWN";
}

std::string orbitClass(const SatelliteRecord& s) {
    std::string orbit = s.apogeeKm < 2000 ? "LEO" : (std::abs(s.periodMinutes - 1436.0) < 35 ? "GEO" : (s.apogeeKm < 35000 ? "MEO" : "HEO"));
    if (s.inclinationDeg > 80 && s.inclinationDeg < 100) orbit += " / POLAR";
    return orbit;
}

std::vector<SatelliteRecord> parseSatcat(const Response& response) {
    std::vector<SatelliteRecord> out;
    if (response.body.empty()) return out;
    for (const auto& row : json::parse(response.body)) {
        SatelliteRecord s;
        s.norad = static_cast<int>(num(row, "NORAD_CAT_ID"));
        s.name = str(row, "OBJECT_NAME"); s.internationalId = str(row, "OBJECT_ID");
        s.objectType = str(row, "OBJECT_TYPE"); s.opsCode = str(row, "OPS_STATUS_CODE", "");
        s.ownerCode = str(row, "OWNER"); s.launchDate = str(row, "LAUNCH_DATE");
        s.launchSite = str(row, "LAUNCH_SITE"); s.periodMinutes = num(row, "PERIOD");
        s.inclinationDeg = num(row, "INCLINATION"); s.apogeeKm = num(row, "APOGEE");
        s.perigeeKm = num(row, "PERIGEE"); s.radarCrossSection = num(row, "RCS", -1);
        if (s.norad > 0) out.push_back(std::move(s));
    }
    return out;
}

std::set<int> parseGroup(const Response& response) {
    std::set<int> ids;
    if (response.body.empty()) return ids;
    for (const auto& row : json::parse(response.body)) ids.insert(static_cast<int>(num(row, "NORAD_CAT_ID")));
    return ids;
}

std::vector<CloseApproach> parseCad(const Response& response) {
    std::vector<CloseApproach> out;
    if (response.body.empty()) return out;
    const auto root = json::parse(response.body);
    std::map<std::string, std::size_t> index;
    auto fields = root.value("fields", std::vector<std::string>{});
    for (std::size_t i = 0; i < fields.size(); ++i) index[fields[i]] = i;
    for (const auto& row : root.value("data", json::array())) {
        auto get = [&](const char* key) { auto i = index.find(key); return i == index.end() || row[i->second].is_null() ? std::string("0") : row[i->second].get<std::string>(); };
        try { out.push_back({get("des"), get("cd"), std::stod(get("dist")), std::stod(get("v_rel")), std::stod(get("h"))}); } catch (...) { }
    }
    return out;
}

Snapshot fetchInitial() {
    Snapshot result;
    const auto satcat = cachedGet("satcat-active", "https://celestrak.org/satcat/records.php?GROUP=active");
    const auto cad = cachedGet("jpl-cad-60d", "https://ssd-api.jpl.nasa.gov/cad.api?date-min=now&date-max=%2B60&dist-max=0.2&limit=500&sort=date");
    result.satellites = parseSatcat(satcat);
    result.approaches = parseCad(cad);

    const std::vector<std::pair<std::string, std::string>> groups{
        {"STARLINK","starlink"},{"ONEWEB","oneweb"},{"KUIPER","kuiper"},{"GPS","gps-ops"},
        {"GALILEO","galileo"},{"GLONASS","glo-ops"},{"BEIDOU","beidou"},{"WEATHER","weather"},
        {"MILITARY PUBLIC","military"},{"AMATEUR RADIO","amateur"},{"CUBESATS","cubesat"},
        {"EARTH OBS","resource"},{"COMMUNICATIONS","other-comm"},{"SCIENCE","science"}
    };
    std::map<int, std::size_t> byId;
    for (std::size_t i = 0; i < result.satellites.size(); ++i) byId[result.satellites[i].norad] = i;
    for (const auto& [label, group] : groups) {
        const auto response = cachedGet("group-" + group, "https://celestrak.org/NORAD/elements/gp.php?GROUP=" + group + "&FORMAT=json",Payload::Json,std::chrono::hours(24));
        const auto ids = parseGroup(response);
        result.categoryCounts[label] = ids.size();
        for (int id : ids) { auto it = byId.find(id); if (it != byId.end()) result.satellites[it->second].categories.push_back(label); }
    }
    result.feeds.push_back({"SATELLITE CATALOG", "CELESTRAK SATCAT", satcat.live ? "LIVE" : (satcat.cached ? "CACHE" : "OFFLINE"), result.satellites.size()});
    result.feeds.push_back({"CLOSE APPROACH", "JPL CNEOS CAD", cad.live ? "LIVE" : (cad.cached ? "CACHE" : "OFFLINE"), result.approaches.size()});
    result.feeds.push_back({"ORBIT PROPAGATOR", "SGP4 / VALLADO MODEL", "LOCAL", 1});
    result.feeds.push_back({"DEEP SPACE", "JPL HORIZONS ON DEMAND", "READY", 0});
    result.feeds.push_back({"SMALL BODIES", "JPL SBDB ON DEMAND", "READY", 0});
    result.refreshedAt = utcString(unixNow());
    return result;
}

std::vector<SearchResult> horizonsLookup(const std::string& query) {
    std::vector<SearchResult> out;
    const auto response = cachedGet("lookup-" + std::to_string(std::hash<std::string>{}(lower(query))),
        "https://ssd.jpl.nasa.gov/api/horizons_lookup.api?sstr=" + urlEncode(query));
    if (response.body.empty()) return out;
    const auto root = json::parse(response.body);
    for (const auto& row : root.value("result", json::array())) {
        const auto type = str(row, "type", "unknown");
        out.push_back({str(row, "spkid"), str(row, "name"), typeFrom(type), "JPL HORIZONS LOOKUP", str(row, "pdes", str(row, "spkid"))});
    }
    return out;
}

std::pair<std::string, std::string> parseTle(const Response& response) {
    std::istringstream in(response.body); std::string line, l1, l2;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("1 ", 0) == 0) l1 = line;
        if (line.rfind("2 ", 0) == 0) l2 = line;
    }
    return {l1, l2};
}

void add(ObjectCard& card, std::string label, std::string value, std::string units, std::string source) {
    if (value.empty()) value = "UNAVAILABLE";
    card.fields.push_back({std::move(label), std::move(value), std::move(units), std::move(source)});
}

std::string vectorText(Vec3d v) {
    return fixed(v.x, 3) + ", " + fixed(v.y, 3) + ", " + fixed(v.z, 3);
}

std::string today(double offsetDays = 0) {
    const auto value = static_cast<std::time_t>(unixNow() + offsetDays * 86400.0);
    std::tm t{};
#if defined(_WIN32)
    gmtime_s(&t, &value);
#else
    gmtime_r(&value, &t);
#endif
    std::ostringstream out; out << std::put_time(&t, "%Y-%m-%d"); return out.str();
}

std::optional<std::pair<std::string,std::string>> missionMetadata(const std::string& name) {
    const std::string n = lower(name);
    const std::vector<std::tuple<std::string,std::string,std::string>> missions{
        {"voyager 1","1977-09-05","NASA / JPL"},{"voyager 2","1977-08-20","NASA / JPL"},
        {"new horizons","2006-01-19","NASA / APL / SWRI"},{"juno","2011-08-05","NASA / JPL"},
        {"europa clipper","2024-10-14","NASA / JPL"},{"psyche","2023-10-13","NASA / JPL / ASU"},
        {"lucy","2021-10-16","NASA / SWRI"},{"parker solar probe","2018-08-12","NASA / APL"},
        {"solar orbiter","2020-02-10","ESA / NASA"},{"osiris","2016-09-08","NASA / UARIZONA"},
        {"mars reconnaissance orbiter","2005-08-12","NASA / JPL"},{"maven","2013-11-18","NASA / GSFC"},
        {"lunar reconnaissance orbiter","2009-06-18","NASA / GSFC"}
    };
    for (const auto& [key,date,operatorName] : missions) if (n.find(key) != std::string::npos) return std::pair{date,operatorName};
    return std::nullopt;
}

ObjectCard horizonsCard(const SearchResult& result) {
    ObjectCard card; card.id = result.id; card.name = result.name; card.kind = result.kind;
    card.primarySource = "NASA/JPL HORIZONS"; card.subtitle = "GEOMETRIC CARTESIAN STATE / ON-DEMAND EPHEMERIS";
    const int span = result.kind == ObjectKind::Planet ? 400 : 30;
    const int step = result.kind == ObjectKind::Planet ? 10 : 1;
    const std::string center = result.kind == ObjectKind::Planet ? "500@10" : "500@399";
    std::string url = "https://ssd.jpl.nasa.gov/api/horizons.api?format=json&COMMAND=" + urlEncode("'" + result.id + "'") +
        "&OBJ_DATA='YES'&MAKE_EPHEM='YES'&EPHEM_TYPE='VECTORS'&CENTER=" + urlEncode("'" + center + "'") +
        "&START_TIME=" + urlEncode("'" + today() + "'") + "&STOP_TIME=" + urlEncode("'" + today(span) + "'") +
        "&STEP_SIZE=" + urlEncode("'" + std::to_string(step) + " d'") + "&OUT_UNITS='KM-S'&REF_PLANE='FRAME'&VEC_TABLE='3'&CSV_FORMAT='YES'";
    const auto response = cachedGet("horizons-" + result.id + "-" + today(), url);
    if (response.body.empty()) {
        add(card,"POSITION","UNAVAILABLE","","JPL HORIZONS");
        add(card,"VELOCITY","UNAVAILABLE","","JPL HORIZONS");
        return card;
    }
    const auto root = json::parse(response.body);
    const std::string output = str(root, "result", "");
    const auto begin = output.find("$$SOE"), end = output.find("$$EOE");
    if (begin == std::string::npos || end == std::string::npos) {
        add(card,"EPHEMERIS","UNAVAILABLE — HORIZONS RETURNED NO VECTOR TABLE","","JPL HORIZONS"); return card;
    }
    std::istringstream lines(output.substr(begin + 5, end - begin - 5)); std::string line;
    std::vector<std::array<double, 9>> rows;
    std::vector<std::string> epochs;
    while (std::getline(lines, line)) {
        if (line.find(',') == std::string::npos) continue;
        std::vector<std::string> columns; std::istringstream cells(line); std::string cell;
        while (std::getline(cells, cell, ',')) columns.push_back(cell);
        if (columns.size() < 11) continue;
        try {
            std::array<double,9> r{};
            for (int i = 0; i < 9; ++i) r[i] = std::stod(columns[static_cast<std::size_t>(i + 2)]);
            rows.push_back(r); epochs.push_back(columns[1]);
        } catch (...) { }
    }
    if (rows.empty()) { add(card,"EPHEMERIS","UNAVAILABLE — VECTOR PARSE FAILED","","JPL HORIZONS"); return card; }
    const auto& r = rows.front(); card.position = {r[0],r[1],r[2]}; card.velocity = {r[3],r[4],r[5]}; card.hasState = true;
    card.trajectory.frame = result.kind == ObjectKind::Planet ? "ICRF / SUN CENTER" : "ICRF / EARTH CENTER";
    card.trajectory.units = "km"; card.trajectory.source = "NASA/JPL HORIZONS";
    for (const auto& row : rows) card.trajectory.points.push_back({row[0],row[1],row[2]});
    add(card,"HORIZONS ID",result.id,"","JPL HORIZONS LOOKUP");
    add(card,"EPOCH",epochs.front(),"TDB","JPL HORIZONS");
    add(card,"POSITION XYZ",vectorText(card.position),"km", "JPL HORIZONS / ICRF");
    add(card,"VELOCITY XYZ",vectorText(card.velocity),"km/s", "JPL HORIZONS / ICRF");
    add(card,result.kind == ObjectKind::Planet ? "DISTANCE FROM SUN" : "DISTANCE FROM EARTH",fixed(r[7],3),"km","JPL HORIZONS");
    add(card,"RANGE RATE",fixed(r[8],6),"km/s","JPL HORIZONS");
    add(card,"TRAJECTORY",std::to_string(rows.size()) + " AUTHORITATIVE VECTOR SAMPLES","","JPL HORIZONS");
    if (auto mission = missionMetadata(card.name)) {
        add(card,"MISSION LAUNCH",mission->first,"","NASA/ESA MISSION PROFILE STATIC METADATA");
        add(card,"MISSION OPERATOR",mission->second,"","NASA/ESA MISSION PROFILE STATIC METADATA");
        int y=0,m=0,d=0;
        if (std::sscanf(mission->first.c_str(),"%d-%d-%d",&y,&m,&d)==3) {
            using namespace std::chrono;
            const auto launch=sys_days{year{y}/month{static_cast<unsigned>(m)}/day{static_cast<unsigned>(d)}};
            const double launchUnix=duration<double>(launch.time_since_epoch()).count();
            add(card,"MISSION ELAPSED",fixed((unixNow()-launchUnix)/86400.0,1),"days","LOCAL CLOCK + MISSION LAUNCH DATE");
        }
    } else {
        add(card,"MISSION ELAPSED","UNAVAILABLE — LAUNCH METADATA NOT CONFIGURED","","SOURCE LIMITATION");
    }
    add(card,"TELEMETRY","NOT PUBLIC IN HORIZONS","","SOURCE LIMITATION");
    add(card,"3D GEOMETRY","UNAVAILABLE — POSITION MARKER ONLY","","NO AUTHORITATIVE MODEL INGESTED");
    return card;
}

std::string phys(const json& root, const std::string& key, std::string fallback = "UNAVAILABLE") {
    for (const auto& row : root.value("phys_par", json::array())) if (str(row,"name","") == key) return str(row,"value",fallback);
    return fallback;
}

ObjectCard sbdbCard(const SearchResult& result) {
    ObjectCard card; card.id = result.lookupKey; card.name = result.name; card.kind = result.kind;
    card.primarySource = "NASA/JPL SBDB"; card.subtitle = "SMALL-BODY ORBIT SOLUTION / PHYSICAL PARAMETERS";
    const std::string url = "https://ssd-api.jpl.nasa.gov/sbdb.api?sstr=" + urlEncode(result.lookupKey) + "&phys-par=1&ca-data=1&ca-time=both&vi-data=1&discovery=1&full-prec=1";
    const auto response = cachedGet("sbdb-" + std::to_string(std::hash<std::string>{}(result.lookupKey)), url);
    if (response.body.empty()) { add(card,"SBDB QUERY","UNAVAILABLE","","JPL SBDB"); return card; }
    const auto root = json::parse(response.body); const auto& object = root["object"]; const auto& orbit = root["orbit"];
    card.name = str(object,"fullname",card.name); card.id = str(object,"spkid",card.id);
    add(card,"SPK ID",card.id,"","JPL SBDB");
    add(card,"ORBIT CLASS",str(object.contains("orbit_class") ? object["orbit_class"] : json::object(),"name"),"","JPL SBDB");
    add(card,"HAZARD RATING",str(object,"pha","UNKNOWN") == "YES" ? "POTENTIALLY HAZARDOUS" : "NOT CLASSIFIED PHA","","JPL SBDB");
    add(card,"MOID",str(orbit,"moid"),"au","JPL SBDB");
    add(card,"ORBIT CONDITION",str(orbit,"condition_code"),"code 0-9","JPL SBDB");
    add(card,"ORBIT EPOCH",str(orbit,"epoch"),"JDTDB","JPL SBDB");
    std::map<std::string,double> elements;
    for (const auto& e : orbit.value("elements", json::array())) {
        const auto name = str(e,"name",""); const auto value = str(e,"value","");
        if (!name.empty()) { try { elements[name] = std::stod(value); } catch (...) { } }
        if (name=="a"||name=="e"||name=="i"||name=="om"||name=="w"||name=="ma"||name=="per")
            add(card,"ORBIT " + str(e,"label",name),value,str(e,"units",""),"JPL SBDB");
    }
    card.trajectory = smallBodyOrbit(elements);
    add(card,"SIZE / DIAMETER",phys(root,"diameter"),"km","JPL SBDB PHYSICAL PARAMETERS");
    add(card,"SHAPE / EXTENT",phys(root,"extent"),"km","JPL SBDB PHYSICAL PARAMETERS");
    add(card,"ROTATION PERIOD",phys(root,"rot_per"),"h","JPL SBDB PHYSICAL PARAMETERS");
    add(card,"COMPOSITION","UNAVAILABLE — TAXONOMY IS NOT DIRECT COMPOSITION","","SOURCE LIMITATION");
    add(card,"SPECTRAL TYPE",phys(root,"spec_B",phys(root,"spec_T")),"","JPL SBDB PHYSICAL PARAMETERS");
    if (root.contains("discovery")) add(card,"DISCOVERY",str(root["discovery"],"discovery"),"","JPL SBDB DISCOVERY.DB");
    else add(card,"DISCOVERY","UNAVAILABLE","","JPL SBDB");
    const json* future = nullptr;
    for (const auto& ca : root.value("ca_data", json::array())) {
        if (str(ca,"body","") == "Earth" && num(ca,"jd") >= julianDate(unixNow())) { future = &ca; break; }
    }
    if (future) {
        add(card,"CLOSEST APPROACH",str(*future,"cd"),"","JPL SBDB");
        add(card,"APPROACH DISTANCE",str(*future,"dist"),"au","JPL SBDB");
        add(card,"RELATIVE VELOCITY",str(*future,"v_rel"),"km/s","JPL SBDB");
    } else {
        add(card,"CLOSEST APPROACH","NO FUTURE EARTH APPROACH IN RESPONSE","","JPL SBDB");
        add(card,"RELATIVE VELOCITY","UNAVAILABLE","","JPL SBDB");
    }
    const auto vi = root.value("vi_data", json::array());
    if (!vi.empty()) {
        add(card,"IMPACT PROBABILITY",str(vi.front(),"ip"),"","JPL SENTRY VIA SBDB");
        add(card,"PALERMO SCALE",str(vi.front(),"ps"),"","JPL SENTRY VIA SBDB");
        add(card,"TORINO SCALE",str(vi.front(),"ts"),"","JPL SENTRY VIA SBDB");
    } else {
        add(card,"IMPACT PROBABILITY","NOT LISTED BY SENTRY","","JPL SENTRY VIA SBDB");
        add(card,"PALERMO SCALE","NOT LISTED BY SENTRY","","JPL SENTRY VIA SBDB");
        add(card,"TORINO SCALE","NOT LISTED BY SENTRY","","JPL SENTRY VIA SBDB");
    }
    add(card,"3D GEOMETRY",phys(root,"extent") == "UNAVAILABLE" ? "POINT MARKER — SHAPE UNKNOWN" : "EXTENT AVAILABLE; MESH NOT INGESTED","","JPL SBDB");
    return card;
}

const std::vector<SearchResult>& coreCatalog() {
    static const std::vector<SearchResult> items{
        {"10","Sun",ObjectKind::Planet,"JPL HORIZONS","10"},{"199","Mercury",ObjectKind::Planet,"JPL HORIZONS","199"},
        {"299","Venus",ObjectKind::Planet,"JPL HORIZONS","299"},{"399","Earth",ObjectKind::Planet,"JPL HORIZONS","399"},
        {"499","Mars",ObjectKind::Planet,"JPL HORIZONS","499"},{"599","Jupiter",ObjectKind::Planet,"JPL HORIZONS","599"},
        {"699","Saturn",ObjectKind::Planet,"JPL HORIZONS","699"},{"799","Uranus",ObjectKind::Planet,"JPL HORIZONS","799"},
        {"899","Neptune",ObjectKind::Planet,"JPL HORIZONS","899"},
        {"","Voyager 1",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Voyager 1"},
        {"","Voyager 2",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Voyager 2"},
        {"","New Horizons",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","New Horizons"},
        {"","Juno",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Juno"},
        {"","Europa Clipper",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Europa Clipper"},
        {"","Psyche",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Psyche"},
        {"","Lucy",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Lucy"},
        {"","Parker Solar Probe",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Parker Solar Probe"},
        {"","Solar Orbiter",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Solar Orbiter"},
        {"","OSIRIS-APEX",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","OSIRIS-REx"},
        {"","Mars Reconnaissance Orbiter",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Mars Reconnaissance Orbiter"},
        {"","MAVEN",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","MAVEN"},
        {"","Lunar Reconnaissance Orbiter",ObjectKind::Spacecraft,"JPL HORIZONS LOOKUP","Lunar Reconnaissance Orbiter"}
    };
    return items;
}
} // namespace

DataHub::DataHub() { curl_global_init(CURL_GLOBAL_DEFAULT); }
DataHub::~DataHub() { if (pending_.valid()) pending_.wait(); curl_global_cleanup(); }

void DataHub::refresh() {
    if (loading()) return;
    pending_ = std::async(std::launch::async, fetchInitial);
}

bool DataHub::poll() {
    if (!pending_.valid() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    try { snapshot_ = pending_.get(); error_.clear(); } catch (const std::exception& e) { error_ = e.what(); }
    return true;
}

bool DataHub::loading() const { return pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready; }

std::vector<SearchResult> DataHub::search(const std::string& query) const {
    std::vector<SearchResult> out; std::set<std::string> seen;
    for (const auto& sat : snapshot_.satellites) {
        if (containsCi(sat.name,query) || std::to_string(sat.norad) == query) {
            const ObjectKind kind = containsCi(join(sat.categories),"station") || containsCi(sat.name,"ISS") || containsCi(sat.name,"TIANHE") ? ObjectKind::Station : ObjectKind::Satellite;
            out.push_back({std::to_string(sat.norad),sat.name,kind,"CELESTRAK SATCAT",std::to_string(sat.norad)});
            seen.insert("S" + std::to_string(sat.norad));
            if (out.size() >= 60) break;
        }
    }
    for (const auto& item : coreCatalog()) if (containsCi(item.name,query)) { out.push_back(item); seen.insert("J" + item.id + item.name); }
    for (const auto& approach : snapshot_.approaches) if (containsCi(approach.designation,query)) {
        out.push_back({approach.designation,approach.designation,ObjectKind::Asteroid,"JPL CNEOS CAD",approach.designation});
    }
    try {
        for (auto item : horizonsLookup(query)) {
            const std::string key = "J" + item.id + item.name;
            if (seen.insert(key).second) out.push_back(std::move(item));
            if (out.size() >= 100) break;
        }
    } catch (...) { }
    return out;
}

ObjectCard DataHub::inspectSatellite(int norad) const {
    auto it = std::find_if(snapshot_.satellites.begin(), snapshot_.satellites.end(), [&](const SatelliteRecord& s){ return s.norad == norad; });
    ObjectCard card; card.id = std::to_string(norad); card.kind = ObjectKind::Satellite; card.primarySource = "CELESTRAK SATCAT + GP";
    if (it == snapshot_.satellites.end()) { card.name = "UNKNOWN SATELLITE"; add(card,"NORAD ID",card.id,"","USER QUERY"); return card; }
    const auto& s = *it; card.name = s.name;
    card.kind = containsCi(s.name,"ISS") || containsCi(s.name,"TIANHE") || containsCi(s.name,"TIANGONG") ? ObjectKind::Station : ObjectKind::Satellite;
    card.subtitle = "PUBLIC SATELLITE CATALOG / CURRENT GP PROPAGATION";
    add(card,"NAME",s.name,"","CELESTRAK SATCAT");
    add(card,"NORAD ID",std::to_string(s.norad),"","CELESTRAK SATCAT");
    add(card,"INTERNATIONAL ID",s.internationalId,"","CELESTRAK SATCAT");
    add(card,"ORBIT",orbitClass(s),"","DERIVED FROM CELESTRAK SATCAT");
    add(card,"PERIGEE",fixed(s.perigeeKm,1),"km","CELESTRAK SATCAT");
    add(card,"APOGEE",fixed(s.apogeeKm,1),"km","CELESTRAK SATCAT");
    add(card,"INCLINATION",fixed(s.inclinationDeg,3),"deg","CELESTRAK SATCAT");
    add(card,"PERIOD",fixed(s.periodMinutes,3),"min","CELESTRAK SATCAT");
    add(card,"LAUNCH DATE",s.launchDate,"","CELESTRAK SATCAT");
    add(card,"OPERATOR","NOT PUBLIC IN CELESTRAK SATCAT","","SOURCE LIMITATION");
    add(card,"COUNTRY / OWNER CODE",s.ownerCode,"","CELESTRAK SATCAT");
    add(card,"MISSION","UNAVAILABLE — CATEGORY IS NOT A MISSION STATEMENT","","SOURCE LIMITATION");
    add(card,"CATEGORIES",join(s.categories),"","CELESTRAK GP GROUP MEMBERSHIP");
    add(card,"STATUS",opsStatus(s.opsCode),"","CELESTRAK SATCAT OPS CODE");
    add(card,"OBJECT TYPE",s.objectType,"","CELESTRAK SATCAT");
    if (s.radarCrossSection >= 0) add(card,"RADAR CROSS SECTION",fixed(s.radarCrossSection,4),"m2","CELESTRAK SATCAT");

    const auto tleResponse = cachedGet("tle-" + std::to_string(norad), "https://celestrak.org/NORAD/elements/gp.php?CATNR=" + std::to_string(norad) + "&FORMAT=tle", Payload::Tle);
    const auto [line1,line2] = parseTle(tleResponse);
    if (!line1.empty() && !line2.empty()) {
        const auto state = propagateSgp4(s.name,line1,line2,unixNow());
        if (state.valid) {
            card.position = state.positionEciKm; card.velocity = state.velocityEciKms; card.hasState = true;
            add(card,"VELOCITY",fixed(state.speedKms,6),"km/s","CELESTRAK GP + SGP4");
            add(card,"ALTITUDE",fixed(state.altitudeKm,3),"km","CELESTRAK GP + SGP4");
            add(card,"PREDICTED LATITUDE",fixed(state.latitudeDeg,6),"deg","CELESTRAK GP + SGP4");
            add(card,"PREDICTED LONGITUDE",fixed(state.longitudeDeg,6),"deg","CELESTRAK GP + SGP4");
            add(card,"PREDICTED ECI XYZ",vectorText(state.positionEciKm),"km","CELESTRAK GP + SGP4 / TEME");
            add(card,"PREDICTION EPOCH",utcString(unixNow()),"UTC","LOCAL CLOCK");
            card.trajectory = satelliteTrajectory(s.name,line1,line2,unixNow(),s.periodMinutes);
            card.groundTrack = satelliteGroundTrack(s.name,line1,line2,unixNow(),s.periodMinutes);
            add(card,"GROUND TRACK",std::to_string(card.groundTrack.points.size()) + " SGP4 GEODETIC SAMPLES","","CELESTRAK GP + SGP4");
        } else add(card,"PREDICTED POSITION","UNAVAILABLE — SGP4 ERROR: " + state.error,"","SGP4");
    } else add(card,"PREDICTED POSITION","UNAVAILABLE — NO CURRENT GP ELEMENT SET","","CELESTRAK");
    if (card.kind == ObjectKind::Station) {
        add(card,"DOCKING STATUS","UNAVAILABLE — NO AUTHORITATIVE DOCKING FEED CONFIGURED","","SOURCE LIMITATION");
        add(card,"CREW COUNT","UNAVAILABLE — NO AUTHORITATIVE CREW FEED CONFIGURED","","SOURCE LIMITATION");
        add(card,"CREW NAMES","UNAVAILABLE — NO AUTHORITATIVE CREW FEED CONFIGURED","","SOURCE LIMITATION");
    }
    add(card,"3D MODEL","SCHEMATIC MARKER ONLY — PHYSICAL GEOMETRY NOT IN SOURCE","","CELESTRAK SOURCE LIMITATION");
    return card;
}

ObjectCard DataHub::inspect(const SearchResult& result) const {
    if (result.kind == ObjectKind::Satellite || result.kind == ObjectKind::Station) {
        try { return inspectSatellite(std::stoi(result.id)); } catch (...) { return {}; }
    }
    SearchResult resolved = result;
    if (resolved.id.empty()) {
        auto matches = horizonsLookup(resolved.lookupKey);
        auto it = std::find_if(matches.begin(),matches.end(),[&](const SearchResult& r){ return r.kind == resolved.kind; });
        if (it != matches.end()) resolved = *it;
    }
    if (resolved.kind == ObjectKind::Asteroid || resolved.kind == ObjectKind::Comet) return sbdbCard(resolved);
    if (resolved.kind == ObjectKind::Planet || resolved.kind == ObjectKind::Spacecraft) return horizonsCard(resolved);
    ObjectCard card; card.id = resolved.id; card.name = resolved.name; add(card,"DATA","UNAVAILABLE","","NO MATCHED AUTHORITATIVE SOURCE"); return card;
}

} // namespace wd
