# Theater Sound Manager

Theater Sound Manager is a Windows x64 desktop application for music playlists,
scheduled announcements, live-event transitions, loudness normalization, and
Bluetooth control. The application uses SDL2, OpenGL, ImGui, FMOD, spdlog, and
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

Run the complete clean-room workflow for both configurations:

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

Run from the repository root so development builds can resolve `assets/` and
`config/` without duplicating the local media library:

```powershell
.\build\bin\Release\TheaterSoundManager.exe
```

The CMake-generated Visual Studio debugger and the checked-in VS Code launch
configuration already use the repository root as their working directory.

## Install or package

A Release installation is assembled below the disposable build tree:

```powershell
cmake --install build --config Release
```

The default prefix is `build/install`. It contains the executable, SDL2, the
correct FMOD runtime, configuration, project and third-party licenses,
documentation, and local assets.
Set `TSM_INSTALL_ASSETS=OFF` when a lightweight installation without media is
required.

A ZIP package can be produced with:

```powershell
cmake --build build --config Release --target package
```

## Useful options

- `TSM_FMOD_ROOT`: path to the FMOD `api/core` directory.
- `TSM_WARNINGS_AS_ERRORS`: promote project warnings to errors.
- `TSM_INSTALL_ASSETS`: include the local media library in install/package.
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
media.

## License

The project is licensed under the terms in [LICENSE](LICENSE). FMOD and the
locally supplied media remain subject to their respective licenses.
