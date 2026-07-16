# Theater Sound Manager

Theater Sound Manager is a Windows x64 desktop application and versioned CLI for
music playlists, cinema seasonal calendars, scheduled announcements, restart
recovery, fail-closed safety interlocks, live-event transitions, and loudness
normalization. The application uses SDL2, OpenGL, ImGui, FMOD, spdlog, and
nlohmann/json.

## Repository layout

```text
assets/       Runtime fonts and locally supplied media
cmake/        Dependency, compiler, and runtime staging modules
config/       Versioned application configuration
external/     Local proprietary FMOD SDK only
src/          Application and reusable core sources
tests/        GoogleTest unit and logic tests
tools/        Maintenance utilities
build/        The single generated tree; always disposable
```

Visual Studio solution and project files are intentionally not versioned. CMake
is the only supported build entry point and may generate Visual Studio files
inside `build/`.

## Prerequisites

- Windows 10 or newer, x64.
- Visual Studio 2022 with the Desktop development with C++ workload.
- CMake 3.25 or newer.
- FMOD Engine 2.02 Core API for Windows.
- Internet access during the first configure so CMake can download pinned
  open-source dependencies into `build/_deps`.

Follow [external/README.md](external/README.md) to provide FMOD. No other
dependency must be installed or copied into the source tree.

## Build

Configure and build Debug:

```powershell
cmake --preset default
cmake --build --preset debug
```

Build Release:

```powershell
cmake --build --preset release
```

All binaries, generated projects, downloaded dependencies, symbols, and test
metadata remain below `build/`.

## Validate

Run the complete clean-room workflow. It builds and tests Debug and Release,
then validates the Release install and ZIP package:

```powershell
cmake -E remove_directory build
cmake --workflow --preset validate
```

Or run one test configuration after building it:

```powershell
ctest --preset debug
ctest --preset release
```

## Run

Development and installed builds resolve their adjacent configuration and media
independently of the caller's working directory:

```powershell
.\build\bin\Release\TheaterSoundManager.exe
```

The CMake-generated Visual Studio debugger and the checked-in VS Code launch
configuration use `build/runtime/` as their disposable working directory.

## CLI and application integration

The executable also exposes a versioned, machine-readable CLI while preserving
the GUI as its no-argument default:

```powershell
# Inspect the command catalog
.\build\bin\Release\TheaterSoundManager.exe --cli --schema --pretty

# Validate configuration and every referenced media file
.\build\bin\Release\TheaterSoundManager.exe --cli config validate --check-files

# Start the persistent NDJSON host used by another application
.\build\bin\Release\TheaterSoundManager.exe --cli serve
```

Persistent mode keeps FMOD and playback state alive, accepts one JSON request per
line on stdin, and returns one correlated JSON response per line on stdout. Logs
are isolated on stderr. Sound, playlists, announcements, daily and seasonal
schedules, mixer, recovery, safety state, wedding mode, and LUFS diagnostics are
all controllable through the same API.

See [docs/CLI.md](docs/CLI.md) for the full command contract, exit codes,
deployment rules, and integration examples. A dependency-free Python client is
available at [examples/cli_client.py](examples/cli_client.py).

## Cinema deployment

Cinema mode supports absolute dates, recurring annual dates/ranges, Gregorian
Easter offsets, local operating windows, weekday filters, deterministic
priorities, and atomic crash recovery. Its interlock hard-stops normal programme
audio on alarm, fault, timeout, corrupt state, or persistence failure; alarm can
route a protected evacuation preset while faults remain silent.

Read [docs/CINEMA.md](docs/CINEMA.md) before deployment. TSM can complement an
external fire-safety system but is not a certified SSI, general alarm,
emergency-lighting controller, hardware mute, or sole life-safety path.

## Install or package

A Release installation is assembled below the disposable build tree:

```powershell
cmake --install build --config Release
```

The default prefix is `build/install`. It contains the executable, SDL2, the
correct FMOD runtime, required Microsoft runtime libraries, an empty production
configuration, project and open-source dependency notices, and integration
documentation. Local media is deliberately excluded so the package is
reproducible and does not redistribute machine-specific or unlicensed content.

Set `TSM_INSTALL_ASSETS=ON` only for a controlled deployment whose local media
and FMOD redistribution rights have been verified. That mode installs the
populated development configuration together with `assets/`.

A ZIP package can be produced with:

```powershell
cmake --build build --config Release --target package
```

## Useful options

- `TSM_FMOD_ROOT`: path to the FMOD `api/core` directory.
- `TSM_WARNINGS_AS_ERRORS`: promote project warnings to errors.
- `TSM_INSTALL_ASSETS`: include the local media library and populated config;
  defaults to `OFF`.
- `BUILD_TESTING`: build and register the test suite.

Pass options during configuration, for example:

```powershell
cmake --preset default -DTSM_WARNINGS_AS_ERRORS=ON
```

## Clean

The build is deliberately stateless outside one directory:

```powershell
cmake -E remove_directory build
```

Deleting `build/` removes every generated build artifact and downloaded
open-source dependency without touching source files, FMOD, configuration, or
media. Runtime log, layout, and LUFS-cache files live under the current user's
local application data (or the OS temporary directory as a fallback), never in
the repository working directory.

## License

The project is licensed under the terms in [LICENSE](LICENSE). The FMOD SDK,
its runtime, and locally supplied media remain subject to their respective
licenses; consult the terms shipped with the SDK before redistribution.
