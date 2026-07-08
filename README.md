# WATCHDOG

WATCHDOG is a C++20 scientific space-object console that runs directly in macOS Terminal and Windows PowerShell/Windows Terminal.

WATCHDOG never fabricates missing information. Fields absent from a producer’s public data are labeled `UNKNOWN`, `UNAVAILABLE`, or `NOT PUBLIC`.

For a plain-language explanation of every catalog group and common orbital
term, see `OBJECT_CATALOG_GUIDE.txt`.

## Data and calculations



Satellite catalog - CelesTrak SATCAT - Name, NORAD ID, international ID, launch, owner code, operational code, orbit summary 
Satellite elements - CelesTrak GP - Current public TLE selected by NORAD ID 
Satellite propagation - SGP4 - Predicted TEME position/velocity, altitude, geodetic point, orbital path, ground track 
Close approaches - NASA/JPL CNEOS CAD - Upcoming near-Earth encounters, distance and relative velocity 
Asteroids and comets - NASA/JPL SBDB and Sentry fields - Orbit, physical parameters, MOID, discovery, approach and risk values when published 
Planets and spacecraft - NASA/JPL Horizons - ICRF vectors, range, range rate and trajectory samples 
Cross-catalog lookup - NASA/JPL Horizons Lookup - Planets, spacecraft, asteroids, comets and aliases 

Every object-data row includes its source. Satellite trajectory prediction is derived from current public GP elements with SGP4 and is not an operator ephemeris or a conjunction-safety product.

## Build

The first configure downloads pinned C++ dependencies. CMake uses an installed SDL2 and libcurl when available, with source-build fallbacks.

### macOS

Requirements: CMake 3.24+, Git, Xcode Command Line Tools.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/watchdog
```

To launch it globally as `watchdog`:

```bash
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
watchdog
```

### Windows PowerShell

Requirements: Visual Studio 2022 C++ workload, CMake 3.24+, Git. PowerShell inside Windows Terminal is recommended for complete Unicode and ANSI rendering.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\watchdog.exe
```

Optional install:

```powershell
cmake --install build --config Release --prefix "$HOME\watchdog"
$env:Path += ";$HOME\watchdog\bin"
watchdog
```

## Keyboard model

Key | Action 

`/` or `Ctrl+F` - Enter object search 
`:` - Enter command mode 
`Tab` / `Shift+Tab` - Cycle panes 
`j` / `k` or arrows - Move through the active pane 
`Enter` - Inspect selected object/category
`Esc` - Return to normal/search mode 
`Ctrl+R` - Refresh authoritative indexes 
`Ctrl+L` - Force terminal redraw 
`Ctrl+V` - Open selected object in the detached visualizer 
`q` - Exit from normal mode 



## Commands

```text
search <name | NORAD ID | designation>
inspect <result number | NORAD ID | name>
view <object | ID>
track <satellite | NORAD ID>
solar
asteroids
missions
planet <name>
satellite <category>
list <category>
orbit
scale true
scale exaggerated
sources
status
refresh
clear
help
quit
```

Examples:

```text
search iss
inspect 25544
view 25544

missions
inspect Voyager 1
view Voyager 1

planet mars
asteroids
inspect Apophis
```

The visualizer window uses arrow keys to rotate, `+`/`-` or the mouse wheel to zoom, and `Esc` to close. Its title identifies the object, reference frame, and geometry source. A point or schematic reference body is used when no authoritative physical model is present.

## Tests

- `watchdog_sgp4_smoke` checks a known ISS element set for plausible SGP4 altitude and velocity.
- `watchdog_data_smoke` checks live/cached SATCAT ingestion, NORAD 25544 inspection, and populated orbit/ground-track geometry.
