#include "data.hpp"
#include "orbit.hpp"
#include "visualizer.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <future>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace wd {
namespace {

using namespace ftxui;

enum class Pane { Catalog, Objects, Data, Feed, Command };
enum class ViewMode { Orbit, Constellations, NearEarth, Solar, Missions, Search };

struct LogLine {
    std::string time;
    std::string state;
    std::string message;
    bool system{};
    bool warning{};
};

struct JobResult {
    std::vector<SearchResult> results;
    std::optional<ObjectCard> card;
    std::string message;
    bool visualize{};
    bool failed{};
};

struct App {
    DataHub data;
    ViewMode mode{ViewMode::Search};
    Pane pane{Pane::Objects};
    ObjectCard card;
    std::vector<SearchResult> results;
    std::vector<LogLine> log;
    std::future<JobResult> job;
    std::string command;
    std::string executable;
    int selectedResult{};
    int selectedCategory{};
    int selectedField{};
    bool commandMode{};
    bool exaggerated{};
    bool quitting{};
};

const Color kBorder = Color::RGB(92, 95, 105);
const Color kMuted = Color::RGB(118, 120, 132);
const Color kPurple = Color::RGB(196, 158, 236);
const Color kPink = Color::RGB(225, 155, 235);
const Color kRose = Color::RGB(244, 177, 177);
const Color kSelected = Color::RGB(255, 169, 217);
const Color kGreen = Color::RGB(0, 255, 127);
const Color kBackground = Color::RGB(8, 9, 13);
const Color kPlanetBlue = Color::RGB(0, 33, 243);
const Color kUnknown = Color::RGB(207, 38, 21);

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string tail(const std::string& value) {
    const auto split = value.find(' ');
    return split == std::string::npos ? std::string{} : trim(value.substr(split + 1));
}

std::string clip(std::string value, std::size_t width) {
    if (value.size() <= width) return value;
    if (width < 4) return value.substr(0, width);
    return value.substr(0, width - 3) + "...";
}

std::string nowHms() {
    const auto utc = utcString(unixNow());
    const auto pos = utc.find(' ');
    return pos == std::string::npos ? utc : utc.substr(pos + 1, 8);
}

const char* tag(ObjectKind kind) {
    switch (kind) {
        case ObjectKind::Satellite: return "[SAT]";
        case ObjectKind::Station: return "[ISS]";
        case ObjectKind::Asteroid: return "[AST]";
        case ObjectKind::Comet: return "[COM]";
        case ObjectKind::Planet: return "[PLN]";
        case ObjectKind::Spacecraft: return "[SCI]";
        default: return "[---]";
    }
}

const char* modeName(ViewMode mode) {
    switch (mode) {
        case ViewMode::Orbit: return "EARTH ORBIT";
        case ViewMode::Constellations: return "CONSTELLATIONS";
        case ViewMode::NearEarth: return "NEAR-EARTH";
        case ViewMode::Solar: return "SOLAR SYSTEM";
        case ViewMode::Missions: return "MISSIONS";
        default: return "SEARCH / INSPECT";
    }
}

bool isUnknownValue(const std::string& value) {
    const auto text = lower(value);
    return text == "none" || text == "--" ||
           text.find("unknown") != std::string::npos ||
           text.find("unavailable") != std::string::npos ||
           text.find("not public") != std::string::npos ||
           text.find("no object selected") != std::string::npos;
}

Color planetNameColor(const std::string& name) {
    const auto body = lower(name);
    if (body.find("mercury") != std::string::npos) return Color::RGB(188, 188, 188);
    if (body.find("venus") != std::string::npos) return kRose;
    if (body.find("earth") != std::string::npos) return kPlanetBlue;
    if (body.find("mars") != std::string::npos) return Color::RGB(204, 0, 0);
    if (body.find("jupiter") != std::string::npos) return Color::RGB(245, 227, 132);
    if (body.find("saturn") != std::string::npos) return Color::RGB(236, 90, 161);
    if (body.find("uranus") != std::string::npos) return Color::RGB(159, 197, 232);
    if (body.find("neptune") != std::string::npos) return Color::RGB(61, 133, 198);
    return kRose;
}

Color objectNameColor(ObjectKind kind, const std::string& name, bool selected = false) {
    if (kind == ObjectKind::Unknown || isUnknownValue(name)) return kUnknown;
    if (kind == ObjectKind::Planet) return planetNameColor(name);
    return selected ? kSelected : kRose;
}

Color objectTagColor(ObjectKind kind, bool selected = false) {
    if (kind == ObjectKind::Unknown) return kUnknown;
    if (kind == ObjectKind::Planet) return kPlanetBlue;
    return selected ? kSelected : kPurple;
}

void addLog(App& app, std::string state, std::string message,
            bool system = false, bool warning = false) {
    app.log.push_back({nowHms(), std::move(state), std::move(message), system, warning});
    if (app.log.size() > 160) app.log.erase(app.log.begin(), app.log.begin() + 40);
}

bool jobRunning(const App& app) {
    return app.job.valid() &&
           app.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

Element labelValue(const std::string& label, const std::string& value,
                   const std::string& units = {}, std::optional<Color> overrideColor = std::nullopt) {
    const Color valueColor = overrideColor.value_or(isUnknownValue(value) ? kUnknown : kPink);
    return hbox({text(clip(label, 18)) | size(WIDTH, EQUAL, 19) | color(kPurple),
                 text(" : ") | color(kMuted),
                 text(clip(value + (units.empty() ? "" : " " + units), 34)) | color(valueColor)});
}

Element paneWindow(const std::string& title, Element content, bool active = false) {
    auto heading = text(" " + title + " ") | bold | color(active ? kSelected : kPurple);
    return window(std::move(heading), std::move(content)) | color(kBorder);
}

std::vector<std::pair<std::string, std::size_t>> categories(const App& app) {
    std::vector<std::pair<std::string, std::size_t>> out;
    for (const auto& [name, count] : app.data.snapshot().categoryCounts) {
        out.emplace_back(name, count);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Element renderCatalog(const App& app) {
    Elements lines;
    const auto cats = categories(app);
    if (cats.empty()) {
        lines.push_back(text("CATALOG SYNCHRONIZING") | color(kPurple));
        lines.push_back(text("CelesTrak SATCAT / GP") | color(kMuted));
    } else {
        for (std::size_t i = 0; i < cats.size(); ++i) {
            const bool selected = static_cast<int>(i) == app.selectedCategory;
            auto row = hbox({text(selected ? "> " : "  ") | color(kSelected),
                             text(clip(cats[i].first, 17)) | flex | color(selected ? kSelected : kPurple),
                             text(std::to_string(cats[i].second)) | color(kPink)});
            if (selected) row = row | bgcolor(Color::RGB(27, 22, 36));
            lines.push_back(std::move(row));
        }
    }
    lines.push_back(separator() | color(kBorder));
    lines.push_back(text("MAIN STUFF") | color(kRose) | bold);
    for (const auto& feed : app.data.snapshot().feeds) {
        const bool good = feed.state == "READY" || feed.state == "ONLINE" || feed.state == "CACHED";
        lines.push_back(hbox({text(clip(feed.label, 16)) | flex | color(kPurple),
                             text(feed.state) | color(good ? kGreen : kRose)}));
        lines.push_back(text("  " + clip(feed.source, 25)) | color(kMuted));
    }
    return paneWindow("OBJECTS", vbox(std::move(lines)) | vscroll_indicator | frame,
                      app.pane == Pane::Catalog);
}

Element renderObjects(const App& app) {
    Elements lines;
    lines.push_back(hbox({text("TYPE ") | size(WIDTH, EQUAL, 7) | color(kPurple),
                          text("IDENTIFIER") | size(WIDTH, EQUAL, 13) | color(kPurple),
                          text("OBJECT") | flex | color(kPurple),
                          text("SOURCE") | size(WIDTH, EQUAL, 17) | color(kPurple)}));
    lines.push_back(separator() | color(kBorder));
    if (app.results.empty()) {
        lines.push_back(text("No result set. Press / and enter a search query.") | color(kMuted));
        lines.push_back(text("Examples: ISS, 25544, Voyager 1, Apophis, Mars") | color(kPurple));
    }
    for (std::size_t i = 0; i < app.results.size(); ++i) {
        const auto& result = app.results[i];
        const bool selected = static_cast<int>(i) == app.selectedResult;
        auto row = hbox({text(selected ? "> " : "  ") | color(kSelected),
                         text(tag(result.kind)) | size(WIDTH, EQUAL, 7) | color(objectTagColor(result.kind, selected)),
                         text(clip(result.id.empty() ? "--" : result.id, 11)) | size(WIDTH, EQUAL, 13) |
                             color(result.id.empty() ? kUnknown : kPink),
                         text(clip(result.name, 31)) | flex | color(objectNameColor(result.kind, result.name, selected)),
                         text(clip(result.source, 17)) | size(WIDTH, EQUAL, 17) | color(kMuted)});
        if (selected) row = row | bgcolor(Color::RGB(31, 23, 38));
        lines.push_back(std::move(row));
    }
    return paneWindow("MENU",
                      vbox(std::move(lines)) | vscroll_indicator | frame,
                      app.pane == Pane::Objects);
}

Element renderObjectData(const App& app) {
    Elements lines;
    lines.push_back(text(clip(app.card.name, 42)) |
                    color(objectNameColor(app.card.kind, app.card.name)) | bold);
    lines.push_back(hbox({text(tag(app.card.kind)) | color(objectTagColor(app.card.kind)), text("  "),
                          text(kindName(app.card.kind)) | color(kPurple), text("  ID ") | color(kMuted),
                          text(app.card.id.empty() ? "UNAVAILABLE" : app.card.id) |
                              color(app.card.id.empty() ? kUnknown : kPink)}));
    lines.push_back(text(clip(app.card.subtitle, 48)) | color(kMuted));
    lines.push_back(separator() | color(kBorder));
    lines.push_back(hbox({text("PRIMARY SOURCE") | color(kPurple), text(" : ") | color(kMuted),
                          text(clip(app.card.primarySource, 31)) |
                              color(isUnknownValue(app.card.primarySource) ? kUnknown : kRose)}));
    lines.push_back(separator() | color(kBorder));
    if (app.card.fields.empty()) {
        lines.push_back(text("No object selected.") | color(kMuted));
        lines.push_back(text("Enter: inspect result  |  Ctrl+V: visualize") | color(kPurple));
    }
    for (std::size_t i = 0; i < app.card.fields.size(); ++i) {
        const auto& field = app.card.fields[i];
        const bool selected = static_cast<int>(i) == app.selectedField;
        const auto fieldColor = app.card.kind == ObjectKind::Planet && lower(field.label) == "name"
                                    ? std::optional<Color>{planetNameColor(app.card.name)}
                                    : std::nullopt;
        auto value = labelValue(field.label, field.value, field.units, fieldColor);
        if (selected && app.pane == Pane::Data) value = value | bgcolor(Color::RGB(29, 23, 36));
        lines.push_back(std::move(value));
        lines.push_back(text("   SRC: " + clip(field.source, 40)) | color(kMuted));
    }
    lines.push_back(separator() | color(kBorder));
    lines.push_back(labelValue("TRAJECTORY SAMPLES", std::to_string(app.card.trajectory.points.size())));
    lines.push_back(labelValue("REFERENCE FRAME", app.card.trajectory.frame));
    lines.push_back(labelValue("GEOMETRY SOURCE", app.card.trajectory.source));
    return paneWindow("DATA", vbox(std::move(lines)) | vscroll_indicator | frame,
                      app.pane == Pane::Data);
}

Element renderTelemetry(const App& app) {
    const auto& snapshot = app.data.snapshot();
    Elements lines;
    const std::string state = app.data.loading() ? "SYNC" : (app.data.error().empty() ? "LIVE" : "DEGRADED");
    lines.push_back(hbox({text("UTC ") | color(kPurple), text(utcString(unixNow())) | color(kPink),
                          text("   JULIAN ") | color(kPurple),
                          text([&] { std::ostringstream s; s << std::fixed << std::setprecision(5) << julianDate(unixNow()); return s.str(); }()) | color(kPink),
                          filler(), text(state) | color(state == "LIVE" ? kGreen : kRose)}));
    lines.push_back(separator() | color(kBorder));
    lines.push_back(hbox({text("SATELLITES ") | color(kPurple), text(std::to_string(snapshot.satellites.size())) | color(kPink),
                          text("   CLOSE APPROACHES ") | color(kPurple), text(std::to_string(snapshot.approaches.size())) | color(kPink),
                          text("   VIEW ") | color(kPurple), text(app.exaggerated ? "EXAGGERATED / LABELED" : "LINEAR SCALE") | color(kPink)}));
    if (!snapshot.approaches.empty()) {
        const auto& approach = snapshot.approaches.front();
        std::ostringstream distance;
        distance << std::scientific << std::setprecision(3) << approach.distanceAu;
        lines.push_back(hbox({text("NEXT CAD ") | color(kPurple), text(approach.designation) | color(kRose),
                              text("  ") , text(approach.date) | color(kPink),
                              text("  DIST ") | color(kPurple), text(distance.str() + " au") | color(kPink),
                              text("  VREL ") | color(kPurple), text(std::to_string(approach.velocityKms) + " km/s") | color(kPink)}));
    }
    lines.push_back(hbox({text("VISUALIZER ") | color(kPurple),
                          text("DETACHED SDL2 / OPENGL  [Ctrl+V]") | color(kMuted),
                          filler(), text("TERMINAL REMAINS COMMAND AUTHORITY") | color(kPurple)}));
    return paneWindow("DATE AND TIME", vbox(std::move(lines)), false);
}

Element renderFeed(const App& app) {
    Elements lines;
    const std::size_t begin = app.log.size() > 6 ? app.log.size() - 6 : 0;
    for (std::size_t i = begin; i < app.log.size(); ++i) {
        const auto& line = app.log[i];
        const Color stateColor = line.system ? kGreen : (line.warning ? kRose : kPurple);
        lines.push_back(hbox({text(line.time) | size(WIDTH, EQUAL, 10) | color(kMuted),
                              text(clip(line.state, 13)) | size(WIDTH, EQUAL, 15) | color(stateColor),
                              text(clip(line.message, 100)) | flex | color(line.warning ? kRose : kPink)}));
    }
    if (lines.empty()) lines.push_back(text("WATCHDOG console initialized.") | color(kMuted));
    return paneWindow("LIVE FEED", vbox(std::move(lines)), app.pane == Pane::Feed);
}

Element renderHeader(const App& app) {
    const std::string api = app.data.loading() ? "SYNCHRONIZING" : (app.data.error().empty() ? "API READY" : "API DEGRADED");
    return hbox({text(" SPACE OBJECT COMMAND CONSOLE ") | bold | color(kPurple),
                 text("  //  ") | color(kBorder), text(modeName(app.mode)) | color(kSelected),
                 filler(), text(api) | color(app.data.error().empty() ? kGreen : kRose),
                 text("  UTC ") | color(kPurple), text(nowHms()) | color(kPink), text(" ")}) |
           bgcolor(kBackground);
}

Element renderTabs(const App& app) {
    const std::vector<std::pair<ViewMode, std::string>> tabs{{ViewMode::Orbit, "ORBIT"},
        {ViewMode::Constellations, "CONSTELLATIONS"}, {ViewMode::NearEarth, "ASTEROIDS"},
        {ViewMode::Solar, "SOLAR"}, {ViewMode::Missions, "MISSIONS"}, {ViewMode::Search, "SEARCH"}};
    Elements elements;
    for (const auto& [mode, name] : tabs) {
        elements.push_back(text(" " + name + " ") | color(app.mode == mode ? kSelected : kPurple) |
                           (app.mode == mode ? bold : nothing));
        elements.push_back(text("│") | color(kBorder));
    }
    elements.push_back(filler());
    elements.push_back(text("/ search  Tab panes  Enter inspect  Ctrl+V view  : command ") | color(kMuted));
    return hbox(std::move(elements));
}

Element renderScreen(const App& app, Element commandInput) {
    const auto dimensions = Terminal::Size();
    if (dimensions.dimx < 118 || dimensions.dimy < 30) {
        return vbox({filler(),
                     hbox({filler(),
                           window(text(" WATCHDOG // TERMINAL GEOMETRY ") | color(kRose) | bold,
                                  vbox({text("Terminal is too small for the scientific console.") | color(kPurple),
                                        text("Required : 118 columns x 30 rows") | color(kPink),
                                        text("Current  : " + std::to_string(dimensions.dimx) + " columns x " +
                                             std::to_string(dimensions.dimy) + " rows") | color(kPink),
                                        separator() | color(kBorder),
                                        text("Resize the terminal; WATCHDOG will redraw automatically.") | color(kGreen)})) |
                               color(kBorder),
                           filler()}),
                     filler()}) | bgcolor(kBackground);
    }
    auto main = hbox({renderCatalog(app) | size(WIDTH, EQUAL, 29),
                      vbox({renderObjects(app) | flex, renderTelemetry(app) | size(HEIGHT, EQUAL, 7)}) | flex,
                      renderObjectData(app) | size(WIDTH, EQUAL, 52)}) | flex;
    auto command = hbox({text(app.commandMode ? " COMMAND > " : " NORMAL  > ") |
                             color(app.commandMode ? kSelected : kPurple) | bold,
                         std::move(commandInput) | flex,
                         text(jobRunning(app) ? " BUSY " : " READY ") |
                             color(jobRunning(app) ? kPink : kGreen)}) |
                   borderStyled(ROUNDED, app.pane == Pane::Command ? kSelected : kBorder);
    return vbox({renderHeader(app), separator() | color(kBorder), renderTabs(app),
                 separator() | color(kBorder), std::move(main),
                 renderFeed(app) | size(HEIGHT, EQUAL, 8), std::move(command) | size(HEIGHT, EQUAL, 3)}) |
           bgcolor(kBackground);
}

std::optional<SearchResult> resolveTarget(const App& app, const std::string& target) {
    if (target.empty() && !app.results.empty()) return app.results[std::clamp(app.selectedResult, 0, static_cast<int>(app.results.size()) - 1)];
    try {
        const int value = std::stoi(target);
        if (value >= 1 && value <= static_cast<int>(app.results.size())) return app.results[value - 1];
        for (const auto& satellite : app.data.snapshot().satellites) {
            if (satellite.norad == value) {
                return SearchResult{target, satellite.name, ObjectKind::Satellite,
                                    "CELESTRAK SATCAT", target};
            }
        }
    } catch (...) {
    }
    const auto needle = lower(target);
    for (const auto& result : app.results) {
        if (lower(result.name).find(needle) != std::string::npos) return result;
    }
    return std::nullopt;
}

void startSearch(App& app, std::string query, bool inspect, bool visualize,
                 std::optional<ObjectKind> preferred = std::nullopt) {
    if (jobRunning(app)) {
        addLog(app, "BUSY", "Wait for the active authoritative query.", false, true);
        return;
    }
    query = trim(query);
    if (query.empty()) {
        addLog(app, "QUERY ERROR", "A search term or catalog identifier is required.", false, true);
        return;
    }
    addLog(app, "QUERY", query + "  -> authoritative catalogs");
    const DataHub* data = &app.data;
    app.job = std::async(std::launch::async, [data, query, inspect, visualize, preferred] {
        JobResult output;
        try {
            output.results = data->search(query);
            if (inspect && !output.results.empty()) {
                auto selected = output.results.begin();
                if (preferred) {
                    const auto match = std::find_if(output.results.begin(), output.results.end(),
                                                    [&](const SearchResult& item) { return item.kind == *preferred; });
                    if (match != output.results.end()) selected = match;
                }
                output.card = data->inspect(*selected);
                output.visualize = visualize;
            }
            output.message = std::to_string(output.results.size()) + " object(s) returned";
        } catch (const std::exception& error) {
            output.failed = true;
            output.message = error.what();
        }
        return output;
    });
}

void startInspect(App& app, SearchResult result, bool visualize) {
    if (jobRunning(app)) {
        addLog(app, "BUSY", "Wait for the active authoritative query.", false, true);
        return;
    }
    addLog(app, "IDENTIFY", result.name + "  [" + result.source + "]");
    const DataHub* data = &app.data;
    app.job = std::async(std::launch::async, [data, result = std::move(result), visualize] {
        JobResult output;
        try {
            output.card = data->inspect(result);
            output.visualize = visualize;
            output.message = output.card->name;
        } catch (const std::exception& error) {
            output.failed = true;
            output.message = error.what();
        }
        return output;
    });
}

void launchCurrent(App& app) {
    std::string error;
    if (launchVisualizer(app.executable, app.card, app.exaggerated, error)) {
        addLog(app, "LINK ESTABLISHED", "Detached SDL2/OpenGL view: " + app.card.name, true);
    } else {
        addLog(app, "VISUALIZER", error, false, true);
    }
}

std::vector<SearchResult> missionCatalog() {
    const std::vector<std::string> names{"Voyager 1", "Voyager 2", "New Horizons", "Juno",
        "Europa Clipper", "Psyche", "Lucy", "Parker Solar Probe", "Solar Orbiter",
        "OSIRIS-APEX", "Mars Reconnaissance Orbiter", "MAVEN", "Lunar Reconnaissance Orbiter"};
    std::vector<SearchResult> out;
    for (const auto& name : names) out.push_back({"", name, ObjectKind::Spacecraft, "JPL HORIZONS LOOKUP", name});
    return out;
}

std::vector<SearchResult> planetCatalog() {
    const std::vector<std::string> names{"Sun", "Mercury", "Venus", "Earth", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune"};
    std::vector<SearchResult> out;
    for (const auto& name : names) out.push_back({"", name, ObjectKind::Planet, "JPL HORIZONS LOOKUP", name});
    return out;
}

void listCategory(App& app, const std::string& query) {
    app.results.clear();
    const auto needle = lower(query);
    for (const auto& sat : app.data.snapshot().satellites) {
        bool match = needle.empty();
        for (const auto& category : sat.categories) {
            if (lower(category).find(needle) != std::string::npos) match = true;
        }
        if (!match) continue;
        app.results.push_back({std::to_string(sat.norad), sat.name, ObjectKind::Satellite,
                               "CELESTRAK GP GROUP", std::to_string(sat.norad)});
        if (app.results.size() >= 250) break;
    }
    app.selectedResult = 0;
    app.mode = ViewMode::Constellations;
    addLog(app, "CATALOG", std::to_string(app.results.size()) + " matching satellite records");
}

void execute(App& app, std::string command) {
    command = trim(command);
    if (command.empty()) return;
    addLog(app, "COMMAND", command);
    const std::string normalized = lower(command);
    const std::string argument = tail(command);

    if (normalized == "help" || normalized == "?") {
        addLog(app, "HELP", "search <object> | inspect <result/id> | view <object/id> | track <satellite>");
        addLog(app, "HELP", "solar | asteroids | missions | planet <name> | satellite <group> | refresh");
        addLog(app, "KEYS", "/ search  Tab pane  j/k move  Enter inspect  Ctrl+V view  Ctrl+R refresh");
    } else if (normalized.rfind("search ", 0) == 0) {
        app.mode = ViewMode::Search;
        startSearch(app, argument, false, false);
    } else if (normalized.rfind("inspect", 0) == 0) {
        if (auto target = resolveTarget(app, argument)) startInspect(app, *target, false);
        else startSearch(app, argument, true, false);
    } else if (normalized.rfind("view", 0) == 0 || normalized.rfind("3d visualize", 0) == 0 ||
               normalized.rfind("track", 0) == 0) {
        const std::string query = normalized.rfind("3d visualize", 0) == 0 ? trim(command.substr(12)) : argument;
        if (query.empty() && app.card.kind != ObjectKind::Unknown) launchCurrent(app);
        else if (auto target = resolveTarget(app, query)) startInspect(app, *target, true);
        else startSearch(app, query, true, true);
    } else if (normalized == "solar") {
        app.mode = ViewMode::Solar;
        app.results = planetCatalog();
        app.selectedResult = 0;
        addLog(app, "CATALOG", "Solar-system bodies staged from JPL Horizons lookup keys.");
    } else if (normalized == "missions") {
        app.mode = ViewMode::Missions;
        app.results = missionCatalog();
        app.selectedResult = 0;
        addLog(app, "CATALOG", "Active/deep-space mission lookup set loaded.");
    } else if (normalized == "asteroids") {
        app.mode = ViewMode::NearEarth;
        app.results.clear();
        for (const auto& approach : app.data.snapshot().approaches) {
            app.results.push_back({approach.designation, approach.designation, ObjectKind::Asteroid,
                                   "JPL CNEOS CAD", approach.designation});
        }
        addLog(app, "CAD LIVE", std::to_string(app.results.size()) + " close-approach records loaded", true);
    } else if (normalized.rfind("planet ", 0) == 0) {
        app.mode = ViewMode::Solar;
        startSearch(app, argument, true, false, ObjectKind::Planet);
    } else if (normalized.rfind("satellite ", 0) == 0 || normalized.rfind("list ", 0) == 0) {
        listCategory(app, argument);
    } else if (normalized == "orbit") {
        app.mode = ViewMode::Orbit;
        listCategory(app, "stations");
    } else if (normalized == "refresh") {
        app.data.refresh();
        addLog(app, "API SYNCHRONIZING", "Refreshing CelesTrak and JPL CNEOS indexes.", true);
    } else if (normalized == "scale true") {
        app.exaggerated = false;
        addLog(app, "SCALE", "Linear coordinate scale selected.");
    } else if (normalized == "scale exaggerated") {
        app.exaggerated = true;
        addLog(app, "SCALE", "Exaggerated view enabled and will be labeled.", false, true);
    } else if (normalized == "clear") {
        app.log.clear();
    } else if (normalized == "status" || normalized == "sources") {
        for (const auto& feed : app.data.snapshot().feeds) {
            addLog(app, feed.state, feed.label + " // " + feed.source,
                   feed.state == "READY" || feed.state == "ONLINE" || feed.state == "CACHED");
        }
    } else if (normalized == "quit" || normalized == "q") {
        app.quitting = true;
    } else {
        addLog(app, "UNKNOWN", "Unknown command. Enter `help` for the command index.", false, true);
    }
}

void handleCompletedJob(App& app) {
    if (!app.job.valid() || app.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    auto result = app.job.get();
    if (!result.results.empty()) {
        app.results = std::move(result.results);
        app.selectedResult = 0;
    }
    if (result.card) {
        app.card = std::move(*result.card);
        app.selectedField = 0;
        addLog(app, "OBJECT IDENTIFIED", app.card.name + " // " + app.card.primarySource, true);
    }
    if (result.failed) addLog(app, "QUERY FAILED", result.message, false, true);
    else addLog(app, "SEARCH COMPLETE", result.message, true);
    if (result.visualize && app.card.kind != ObjectKind::Unknown) launchCurrent(app);
}

void moveSelection(App& app, int delta) {
    if (app.pane == Pane::Catalog) {
        const int count = static_cast<int>(categories(app).size());
        if (count) app.selectedCategory = std::clamp(app.selectedCategory + delta, 0, count - 1);
    } else if (app.pane == Pane::Objects) {
        if (!app.results.empty()) app.selectedResult = std::clamp(app.selectedResult + delta, 0, static_cast<int>(app.results.size()) - 1);
    } else if (app.pane == Pane::Data) {
        if (!app.card.fields.empty()) app.selectedField = std::clamp(app.selectedField + delta, 0, static_cast<int>(app.card.fields.size()) - 1);
    }
}

void activateSelection(App& app) {
    if (app.pane == Pane::Catalog) {
        const auto cats = categories(app);
        if (!cats.empty()) listCategory(app, cats[app.selectedCategory].first);
        app.pane = Pane::Objects;
    } else if (app.pane == Pane::Objects && !app.results.empty()) {
        startInspect(app, app.results[app.selectedResult], false);
        app.pane = Pane::Data;
    }
}

std::string executablePath(const char* argv0) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(argv0, error);
    return error ? std::string(argv0) : absolute.string();
}

} // namespace
} // namespace wd

int main(int argc, char** argv) {
    using namespace wd;
    using namespace ftxui;

    if (argc == 3 && std::string(argv[1]) == "--visualizer") return runVisualizer(argv[2]);

    App app;
    app.executable = executablePath(argv[0]);
    addLog(app, "ONLINE", "Terminal on", true);
    addLog(app, "API SYNCHRONIZING", "Loading CelesTrak SATCAT and JPL CNEOS CAD indexes.", true);
    app.data.refresh();

    auto screen = ScreenInteractive::Fullscreen();
    Component input;
    InputOption inputOptions;
    inputOptions.multiline = false;
    inputOptions.transform = [&](InputState state) {
        auto element = std::move(state.element);
        if (state.is_placeholder) element = element | color(kMuted);
        else element = element | color(kPink);
        if (state.focused && app.commandMode) element = element | focus;
        return element;
    };
    inputOptions.on_enter = [&] {
        const std::string command = app.command;
        app.command.clear();
        app.commandMode = false;
        app.pane = Pane::Objects;
        execute(app, command);
        if (app.quitting) screen.ExitLoopClosure()();
    };
    input = Input(&app.command, "type :help", inputOptions);

    auto renderer = Renderer(input, [&] { return renderScreen(app, input->Render()); });
    auto root = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Custom) {
            handleCompletedJob(app);
            if (!jobRunning(app) && app.data.poll()) {
                addLog(app, "API SYNCHRONIZED",
                       std::to_string(app.data.snapshot().satellites.size()) +
                           " satellites; " + std::to_string(app.data.snapshot().approaches.size()) +
                           " close approaches", true);
                if (!app.data.error().empty()) addLog(app, "API DEGRADED", app.data.error(), false, true);
            }
            return true;
        }
        if (app.commandMode) {
            if (event == Event::Escape) {
                app.commandMode = false;
                app.command.clear();
                app.pane = Pane::Objects;
                return true;
            }
            return false;
        }
        if (event == Event::Character('/') || event == Event::Special(std::string(1, 6))) {
            app.command = "search ";
            app.commandMode = true;
            app.pane = Pane::Command;
            input->TakeFocus();
            return true;
        }
        if (event == Event::Character(':')) {
            app.command.clear();
            app.commandMode = true;
            app.pane = Pane::Command;
            input->TakeFocus();
            return true;
        }
        if (event == Event::Tab) {
            app.pane = static_cast<Pane>((static_cast<int>(app.pane) + 1) % 4);
            return true;
        }
        if (event == Event::TabReverse) {
            app.pane = static_cast<Pane>((static_cast<int>(app.pane) + 3) % 4);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            moveSelection(app, 1);
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            moveSelection(app, -1);
            return true;
        }
        if (event == Event::Return) {
            activateSelection(app);
            return true;
        }
        if (event == Event::Special(std::string(1, 18))) {
            app.data.refresh();
            addLog(app, "API SYNCHRONIZING", "Manual refresh requested.", true);
            return true;
        }
        if (event == Event::Special(std::string(1, 12))) return true;
        if (event == Event::Special(std::string(1, 22))) {
            if (app.card.kind != ObjectKind::Unknown) launchCurrent(app);
            else if (!app.results.empty()) startInspect(app, app.results[app.selectedResult], true);
            else addLog(app, "VISUALIZER", "Select an object first.", false, true);
            return true;
        }
        if (event == Event::Escape) {
            app.mode = ViewMode::Search;
            app.pane = Pane::Objects;
            return true;
        }
        if (event == Event::Character('q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        return event.is_character();
    });

    std::atomic<bool> ticking{true};
    std::thread ticker([&] {
        while (ticking.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            screen.PostEvent(Event::Custom);
        }
    });
    screen.Loop(root);
    ticking = false;
    ticker.join();
    return 0;
}
