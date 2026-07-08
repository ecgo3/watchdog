#include "visualizer.hpp"

#include <nlohmann/json.hpp>

#define SDL_MAIN_HANDLED
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#ifdef __APPLE__
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <processthreadsapi.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wd {
namespace {

using json = nlohmann::json;
constexpr double kPi = 3.14159265358979323846;

json vectorJson(const Vec3d& p) { return json::array({p.x, p.y, p.z}); }

Vec3d vectorFromJson(const json& value) {
    if (!value.is_array() || value.size() != 3) return {};
    return {value[0].get<double>(), value[1].get<double>(), value[2].get<double>()};
}

void drawCircle(float radius, int axis) {
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 128; ++i) {
        const float a = static_cast<float>(2.0 * kPi * i / 128.0);
        const float c = std::cos(a) * radius;
        const float s = std::sin(a) * radius;
        if (axis == 0) glVertex3f(0.0F, c, s);
        if (axis == 1) glVertex3f(c, 0.0F, s);
        if (axis == 2) glVertex3f(c, s, 0.0F);
    }
    glEnd();
}

std::filesystem::path packetPath() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("watchdog-visualizer-" + std::to_string(stamp) + ".json");
}

} // namespace

bool launchVisualizer(const std::string& executable, const ObjectCard& card,
                      bool exaggerated, std::string& error) {
    if (card.kind == ObjectKind::Unknown) {
        error = "No object is selected.";
        return false;
    }
    if (card.trajectory.points.empty() && !card.hasState) {
        error = "No authoritative geometry is available for this object.";
        return false;
    }

    json packet{
        {"name", card.name},
        {"id", card.id},
        {"kind", kindName(card.kind)},
        {"source", card.primarySource},
        {"frame", card.trajectory.frame},
        {"units", card.trajectory.units},
        {"geometry_source", card.trajectory.source},
        {"closed", card.trajectory.closed},
        {"exaggerated", exaggerated},
        {"has_state", card.hasState},
        {"position", vectorJson(card.position)},
        {"trajectory", json::array()},
    };
    for (const auto& point : card.trajectory.points) {
        packet["trajectory"].push_back(vectorJson(point));
    }

    const auto path = packetPath();
    std::ofstream stream(path);
    if (!stream) {
        error = "Unable to write visualization packet: " + path.string();
        return false;
    }
    stream << packet.dump(2);
    stream.close();

#ifdef _WIN32
    std::string command = "\"" + executable + "\" --visualizer \"" + path.string() + "\"";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process)) {
        error = "CreateProcess failed while opening the visualization window.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    const pid_t child = fork();
    if (child < 0) {
        error = "fork() failed while opening the visualization window.";
        return false;
    }
    if (child == 0) {
        const pid_t detached = fork();
        if (detached < 0) _exit(126);
        if (detached == 0) {
            setsid();
            execl(executable.c_str(), executable.c_str(), "--visualizer", path.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
#endif
    return true;
}

int runVisualizer(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) return 2;

    json packet;
    try {
        stream >> packet;
    } catch (...) {
        return 2;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    std::vector<Vec3d> points;
    for (const auto& value : packet.value("trajectory", json::array())) {
        points.push_back(vectorFromJson(value));
    }
    const Vec3d position = vectorFromJson(packet.value("position", json::array()));
    if (points.empty()) points.push_back(position);

    Vec3d center{};
    for (const auto& point : points) {
        center.x += point.x;
        center.y += point.y;
        center.z += point.z;
    }
    const double count = static_cast<double>(points.size());
    center.x /= count;
    center.y /= count;
    center.z /= count;
    double extent = 1.0;
    for (const auto& point : points) {
        extent = std::max({extent, std::abs(point.x - center.x),
                          std::abs(point.y - center.y), std::abs(point.z - center.z)});
    }
    const auto normalized = [&](const Vec3d& point) {
        return Vec3d{(point.x - center.x) / extent, (point.y - center.y) / extent,
                     (point.z - center.z) / extent};
    };

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 3;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    const std::string title = "WATCHDOG // " + packet.value("name", "UNAVAILABLE") +
        " // " + packet.value("frame", "UNAVAILABLE") + " // SOURCE: " +
        packet.value("geometry_source", packet.value("source", "UNAVAILABLE"));
    SDL_Window* window = SDL_CreateWindow(
        title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1100, 760,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        SDL_Quit();
        return 3;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 3;
    }
    SDL_GL_SetSwapInterval(1);

    float yaw = -25.0F;
    float pitch = 25.0F;
    float zoom = 3.2F;
    bool running = true;
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (event.key.keysym.sym == SDLK_LEFT) yaw -= 4.0F;
                if (event.key.keysym.sym == SDLK_RIGHT) yaw += 4.0F;
                if (event.key.keysym.sym == SDLK_UP) pitch -= 4.0F;
                if (event.key.keysym.sym == SDLK_DOWN) pitch += 4.0F;
                if (event.key.keysym.sym == SDLK_EQUALS) zoom -= 0.2F;
                if (event.key.keysym.sym == SDLK_MINUS) zoom += 0.2F;
            }
            if (event.type == SDL_MOUSEWHEEL) zoom -= event.wheel.y * 0.15F;
        }
        zoom = std::clamp(zoom, 1.3F, 12.0F);

        int width = 1;
        int height = 1;
        SDL_GL_GetDrawableSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.025F, 0.027F, 0.040F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        const double aspect = static_cast<double>(width) / std::max(1, height);
        glFrustum(-0.8 * aspect, 0.8 * aspect, -0.8, 0.8, 1.0, 100.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0F, 0.0F, -zoom);
        glRotatef(pitch, 1.0F, 0.0F, 0.0F);
        glRotatef(yaw, 0.0F, 1.0F, 0.0F);

        glLineWidth(1.0F);
        glColor4f(0.627F, 0.514F, 1.0F, 0.22F);
        for (int i = -10; i <= 10; ++i) {
            glBegin(GL_LINES);
            glVertex3f(i / 10.0F, 0.0F, -1.0F);
            glVertex3f(i / 10.0F, 0.0F, 1.0F);
            glVertex3f(-1.0F, 0.0F, i / 10.0F);
            glVertex3f(1.0F, 0.0F, i / 10.0F);
            glEnd();
        }

        if (packet.value("kind", "") == "SATELLITE" ||
            packet.value("kind", "") == "HUMAN SPACEFLIGHT") {
            glColor4f(0.769F, 0.620F, 0.925F, 0.42F);
            drawCircle(0.18F, 0);
            drawCircle(0.18F, 1);
            drawCircle(0.18F, 2);
        }

        glLineWidth(2.0F);
        glColor4f(0.882F, 0.608F, 0.922F, 1.0F);
        glBegin(packet.value("closed", false) ? GL_LINE_LOOP : GL_LINE_STRIP);
        for (const auto& point : points) {
            const auto p = normalized(point);
            glVertex3d(p.x, p.y, p.z);
        }
        glEnd();

        const auto marker = normalized(position);
        glPointSize(9.0F);
        glColor4f(1.0F, 0.663F, 0.851F, 1.0F);
        glBegin(GL_POINTS);
        glVertex3d(marker.x, marker.y, marker.z);
        glEnd();

        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace wd
