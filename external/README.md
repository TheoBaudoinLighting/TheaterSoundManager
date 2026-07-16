# FMOD SDK

Open-source dependencies are downloaded by CMake into `build/_deps` at configure time.
Only the proprietary FMOD Core API must be provided locally.

Install FMOD Engine 2.02 for Windows and either:

1. copy its `api/core` contents to `external/fmod`, or
2. configure with `-DTSM_FMOD_ROOT=C:/path/to/fmod/api/core`.

Expected files:

- `external/fmod/inc/fmod.hpp`
- `external/fmod/inc/fmod_errors.h`
- `external/fmod/lib/x64/fmod_vc.lib` and `fmod.dll`
- `external/fmod/lib/x64/fmodL_vc.lib` and `fmodL.dll`

The SDK is intentionally ignored by Git because it is proprietary and supplied
separately. Review the license included with your FMOD SDK before distributing
its runtime DLLs or a packaged application.
