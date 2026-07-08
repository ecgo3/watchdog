#include "data.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    wd::DataHub data;
    data.refresh();
    while (data.loading()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!data.poll()) { std::cerr << "initial poll failed\n"; return 1; }
    if (data.snapshot().satellites.size() < 1000) { std::cerr << "satcat too small\n"; return 2; }
    const auto card = data.inspectSatellite(25544);
    std::cout << card.name << " fields=" << card.fields.size() << " trajectory=" << card.trajectory.points.size() << '\n';
    for (const auto& f : card.fields) if (f.label.find("PREDICT") != std::string::npos || f.label == "VELOCITY") std::cout << f.label << '=' << f.value << '\n';
    if (!card.hasState || card.trajectory.points.size() < 100 || card.groundTrack.points.size() < 100) return 3;
    return 0;
}
