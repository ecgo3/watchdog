#pragma once

#include "model.hpp"

#include <string>

namespace wd {

// Writes an immutable visualization packet and starts another WATCHDOG process.
// The terminal process never gives up ownership of the console.
bool launchVisualizer(const std::string& executable, const ObjectCard& card,
                      bool exaggerated, std::string& error);

// Entry point used by `watchdog --visualizer <packet.json>`.
int runVisualizer(const std::string& packetPath);

} // namespace wd
