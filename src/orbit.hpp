#pragma once

#include "model.hpp"
#include <string>
#include <vector>

namespace wd {

struct SatelliteState {
    Vec3d positionEciKm{};
    Vec3d velocityEciKms{};
    double latitudeDeg{};
    double longitudeDeg{};
    double altitudeKm{};
    double speedKms{};
    bool valid{};
    std::string error;
};

double unixNow();
double julianDate(double unixTime);
std::string utcString(double unixTime, bool seconds = true);
SatelliteState propagateSgp4(const std::string& name, const std::string& line1,
                             const std::string& line2, double unixTime);
Trajectory satelliteTrajectory(const std::string& name, const std::string& line1,
                                const std::string& line2, double unixTime,
                                double periodMinutes, int samples = 220);
Trajectory satelliteGroundTrack(const std::string& name, const std::string& line1,
                                const std::string& line2, double unixTime,
                                double periodMinutes, int samples = 220);
Trajectory smallBodyOrbit(const std::map<std::string, double>& elements);

} // namespace wd
