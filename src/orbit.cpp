#include "orbit.hpp"

#include <CoordGeodetic.h>
#include <DateTime.h>
#include <SGP4.h>
#include <Tle.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace wd {
namespace {
constexpr double Pi = 3.14159265358979323846;

libsgp4::DateTime dateTime(double unixTime) {
    const auto whole = static_cast<std::time_t>(unixTime);
    std::tm t{};
#if defined(_WIN32)
    gmtime_s(&t, &whole);
#else
    gmtime_r(&whole, &t);
#endif
    const int micros = static_cast<int>((unixTime - std::floor(unixTime)) * 1'000'000.0);
    return libsgp4::DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                             t.tm_hour, t.tm_min, t.tm_sec, micros);
}
} // namespace

double unixNow() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

double julianDate(double unixTime) { return unixTime / 86400.0 + 2440587.5; }

std::string utcString(double unixTime, bool seconds) {
    const auto whole = static_cast<std::time_t>(unixTime);
    std::tm t{};
#if defined(_WIN32)
    gmtime_s(&t, &whole);
#else
    gmtime_r(&whole, &t);
#endif
    std::ostringstream out;
    out << std::put_time(&t, seconds ? "%Y-%m-%d %H:%M:%S UTC" : "%Y-%m-%d %H:%M UTC");
    return out.str();
}

SatelliteState propagateSgp4(const std::string& name, const std::string& line1,
                             const std::string& line2, double unixTime) {
    SatelliteState state;
    try {
        const libsgp4::Tle tle(name, line1, line2);
        const libsgp4::SGP4 propagator(tle);
        const auto eci = propagator.FindPosition(dateTime(unixTime));
        const auto p = eci.Position();
        const auto v = eci.Velocity();
        const auto geo = eci.ToGeodetic();
        state.positionEciKm = {p.x, p.y, p.z};
        state.velocityEciKms = {v.x, v.y, v.z};
        state.latitudeDeg = geo.latitude * 180.0 / Pi;
        state.longitudeDeg = geo.longitude * 180.0 / Pi;
        state.altitudeKm = geo.altitude;
        state.speedKms = v.Magnitude();
        state.valid = true;
    } catch (const std::exception& e) {
        state.error = e.what();
    }
    return state;
}

Trajectory satelliteTrajectory(const std::string& name, const std::string& line1,
                                const std::string& line2, double unixTime,
                                double periodMinutes, int samples) {
    Trajectory path;
    path.frame = "TEME EARTH-CENTERED INERTIAL";
    path.units = "km";
    path.source = "CELESTRAK GP + SGP4";
    path.closed = true;
    if (periodMinutes <= 0.0) periodMinutes = 100.0;
    for (int i = 0; i <= samples; ++i) {
        const double t = unixTime + periodMinutes * 60.0 * i / samples;
        const auto state = propagateSgp4(name, line1, line2, t);
        if (state.valid) path.points.push_back(state.positionEciKm);
    }
    return path;
}

Trajectory satelliteGroundTrack(const std::string& name, const std::string& line1,
                                const std::string& line2, double unixTime,
                                double periodMinutes, int samples) {
    Trajectory path;
    path.frame = "EARTH-FIXED GEODETIC PROJECTION";
    path.units = "km";
    path.source = "CELESTRAK GP + SGP4";
    path.closed = true;
    constexpr double earthRadius = 6378.137;
    if (periodMinutes <= 0.0) periodMinutes = 100.0;
    for (int i = 0; i <= samples; ++i) {
        const double t = unixTime + periodMinutes * 60.0 * i / samples;
        const auto state = propagateSgp4(name, line1, line2, t);
        if (!state.valid) continue;
        const double lat = state.latitudeDeg * Pi / 180.0;
        const double lon = state.longitudeDeg * Pi / 180.0;
        path.points.push_back({earthRadius * std::cos(lat) * std::cos(lon),
                               earthRadius * std::cos(lat) * std::sin(lon),
                               earthRadius * std::sin(lat)});
    }
    return path;
}

Trajectory smallBodyOrbit(const std::map<std::string, double>& e) {
    Trajectory path;
    path.frame = "HELIOCENTRIC ECLIPTIC J2000";
    path.units = "au";
    path.source = "JPL SBDB OSCULATING ELEMENTS";
    path.closed = true;
    auto get = [&](const char* key) { auto it = e.find(key); return it == e.end() ? 0.0 : it->second; };
    const double a = get("a"), ec = get("e"), inc = get("i") * Pi / 180.0;
    const double node = get("om") * Pi / 180.0, arg = get("w") * Pi / 180.0;
    if (a <= 0.0 || ec >= 1.0) return path;
    for (int n = 0; n <= 300; ++n) {
        const double nu = 2.0 * Pi * n / 300.0;
        const double r = a * (1.0 - ec * ec) / (1.0 + ec * std::cos(nu));
        const double u = arg + nu;
        path.points.push_back({
            r * (std::cos(node) * std::cos(u) - std::sin(node) * std::sin(u) * std::cos(inc)),
            r * (std::sin(node) * std::cos(u) + std::cos(node) * std::sin(u) * std::cos(inc)),
            r * std::sin(u) * std::sin(inc)
        });
    }
    return path;
}

} // namespace wd
