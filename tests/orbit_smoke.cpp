#include "orbit.hpp"
#include <cmath>
#include <iostream>

int main() {
    const std::string l1 = "1 25544U 98067A   26188.50835634  .00005806  00000+0  11369-3 0  9990";
    const std::string l2 = "2 25544  51.6304 199.5144 0006687 267.6545  92.3678 15.48933372574901";
    const auto state = wd::propagateSgp4("ISS (ZARYA)", l1, l2, 1783440000.0);
    if (!state.valid) { std::cerr << state.error << '\n'; return 1; }
    if (!std::isfinite(state.altitudeKm) || state.altitudeKm < 300 || state.altitudeKm > 600) {
        std::cerr << "unexpected altitude: " << state.altitudeKm << '\n'; return 2;
    }
    if (!std::isfinite(state.speedKms) || state.speedKms < 7 || state.speedKms > 8.5) {
        std::cerr << "unexpected speed: " << state.speedKms << '\n'; return 3;
    }
    std::cout << "SGP4 OK altitude=" << state.altitudeKm << " km speed=" << state.speedKms << " km/s\n";
    return 0;
}
