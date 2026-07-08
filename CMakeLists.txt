cmake_minimum_required(VERSION 3.24)

project(watchdog VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# FTXUI owns the actual terminal screen. It uses ANSI terminals on macOS and
# the Windows console APIs/virtual-terminal support from PowerShell.
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# The visualization is deliberately a second native window/process. SDL2 owns
# that window while OpenGL draws only measured/predicted geometry.
find_package(SDL2 CONFIG QUIET)
if(NOT TARGET SDL2::SDL2)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(SDL2)
endif()
find_package(OpenGL REQUIRED)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Apache-2.0 SGP4 implementation. Satellite positions are propagated from
# current CelesTrak TLEs with SGP4 rather than a two-body approximation.
FetchContent_Declare(
  sgp4_source
  GIT_REPOSITORY https://github.com/dnwrnr/sgp4.git
  GIT_TAG d176906dbfaf8361eee1f83bd1dc1bacc6582798
  SOURCE_SUBDIR libsgp4
)
FetchContent_MakeAvailable(sgp4_source)

# macOS ships libcurl. On Windows, vcpkg is the fastest path; the fallback
# below builds a compact HTTP-only curl with the platform TLS backend.
find_package(CURL QUIET)
if(NOT CURL_FOUND)
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(HTTP_ONLY ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
  set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
  set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
  if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(
    curl
    URL https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(curl)
endif()

add_executable(watchdog
  src/main.cpp
  src/data.cpp
  src/data.hpp
  src/model.hpp
  src/orbit.cpp
  src/orbit.hpp
  src/visualizer.cpp
  src/visualizer.hpp
)

target_include_directories(watchdog PRIVATE ${sgp4_source_SOURCE_DIR}/libsgp4)
if(APPLE)
  target_compile_definitions(watchdog PRIVATE GL_SILENCE_DEPRECATION)
endif()
target_link_libraries(watchdog PRIVATE
  ftxui::screen
  ftxui::dom
  ftxui::component
  SDL2::SDL2
  OpenGL::GL
  nlohmann_json::nlohmann_json
  CURL::libcurl
  sgp4
)

if(MSVC)
  target_compile_options(watchdog PRIVATE /W4 /permissive-)
else()
  target_compile_options(watchdog PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

install(TARGETS watchdog RUNTIME DESTINATION bin BUNDLE DESTINATION .)

include(CTest)
if(BUILD_TESTING)
  add_executable(watchdog_orbit_smoke tests/orbit_smoke.cpp src/orbit.cpp src/orbit.hpp src/model.hpp)
  target_include_directories(watchdog_orbit_smoke PRIVATE src ${sgp4_source_SOURCE_DIR}/libsgp4)
  target_link_libraries(watchdog_orbit_smoke PRIVATE sgp4)
  add_test(NAME watchdog_sgp4_smoke COMMAND watchdog_orbit_smoke)
  add_executable(watchdog_data_smoke tests/data_smoke.cpp src/data.cpp src/orbit.cpp src/data.hpp src/orbit.hpp src/model.hpp)
  target_include_directories(watchdog_data_smoke PRIVATE src ${sgp4_source_SOURCE_DIR}/libsgp4)
  target_link_libraries(watchdog_data_smoke PRIVATE nlohmann_json::nlohmann_json CURL::libcurl sgp4)
  add_test(NAME watchdog_data_smoke COMMAND watchdog_data_smoke)
endif()
