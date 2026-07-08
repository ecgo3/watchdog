#pragma once

#include <map>
#include <string>
#include <vector>

namespace wd {

enum class ObjectKind { Satellite, Station, Asteroid, Comet, Planet, Spacecraft, Unknown };

struct Vec3d { double x{}, y{}, z{}; };

struct Field {
    std::string label;
    std::string value;
    std::string units;
    std::string source;
};

struct Trajectory {
    std::vector<Vec3d> points;
    std::string frame{"UNAVAILABLE"};
    std::string units{"UNAVAILABLE"};
    std::string source{"UNAVAILABLE"};
    bool closed{};
};

struct ObjectCard {
    std::string id;
    std::string name{"NO OBJECT SELECTED"};
    ObjectKind kind{ObjectKind::Unknown};
    std::string subtitle{"Use SEARCH or INSPECT to select an object."};
    std::string primarySource{"NONE"};
    std::vector<Field> fields;
    Trajectory trajectory;
    Trajectory groundTrack;
    Vec3d position{};
    Vec3d velocity{};
    bool hasState{};
    bool geometryAuthoritative{};
};

struct SearchResult {
    std::string id;
    std::string name;
    ObjectKind kind{ObjectKind::Unknown};
    std::string source;
    std::string lookupKey;
};

inline const char* kindName(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::Satellite: return "SATELLITE";
        case ObjectKind::Station: return "HUMAN SPACEFLIGHT";
        case ObjectKind::Asteroid: return "ASTEROID";
        case ObjectKind::Comet: return "COMET";
        case ObjectKind::Planet: return "PLANET";
        case ObjectKind::Spacecraft: return "SPACECRAFT";
        default: return "UNKNOWN";
    }
}

} // namespace wd
