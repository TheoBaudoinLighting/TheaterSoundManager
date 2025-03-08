# Theater Sound Manager (TSM)

TSM (Theater Sound Manager) is an advanced audio management system designed to enhance sound experiences for events, cinemas, and theaters. It allows seamless playback of music, scheduled announcements, and audio effects with precise timing and control.

## Features

- **Dynamic Playlist Management**: Play randomized music segments with smooth crossfades.
- **Scheduled Announcements**: Automate announcements at predefined times.
- **Crossfading and Ducking**: Smooth transitions between tracks and announcements.
- **FMOD Audio Engine**: High-quality audio processing with advanced DSP effects.
- **User Interface**: Integrated with **ImGui** for an intuitive UI experience.
- **Bluetooth Control** *(Planned Feature)*: Remote control via Bluetooth for mobile and tablet interactions.

## Installation

### Requirements
- **C++ Compiler** (GCC/Clang/MSVC)
- **CMake** (Version 3.18 or higher)
- **FMOD Studio API** (For audio processing)
- **ImGui** (For UI rendering)
- **spdlog** (For logging)

### Build Instructions

```sh
# Clone the repository
git clone https://github.com/yourusername/tsm.git
cd tsm

# Create a build directory and configure the project
mkdir build && cd build
cmake ..

# Compile the project
make -j4
```

## Usage

### Starting the Application
```sh
./tsm
```

### Example Code

```cpp
#include "tsm_announcement_manager.h"

// Schedule an announcement at 19:45
TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(19, 45, "announce_5min_buffet");

// Play an announcement immediately
TSM::AnnouncementManager::GetInstance().PlayAnnouncement("announce_welcome", 0.3f, true, true);

// Stop an active announcement
TSM::AnnouncementManager::GetInstance().StopAnnouncement();
```

## Roadmap
- [x] Playlist Manager
- [x] Announcement Scheduling
- [ ] Bluetooth Remote Control
- [ ] Multi-Zone Audio Support
- [ ] Web Interface for Remote Control

## License
This project is licensed under the **MIT License**.

## Contributing
Contributions are welcome! Please follow the standard GitHub workflow:
1. Fork the repository.
2. Create a feature branch (`feature/my-new-feature`).
3. Commit your changes.
4. Submit a pull request.

## Contact
For any questions or collaboration inquiries, feel free to reach out via GitHub Issues or email: **theo.baudoin30@gmail.com**.
