// tsm_main.cpp

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>

#include <SDL.h>
#include <SDL_opengl.h>
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <fmod.hpp>
#include <fmod_errors.h>
#include <spdlog/spdlog.h>

#include "tsm_logger.h"
#include "imgui/tsm_imgui.h"
#include "tsm_announcement.h"
#include "tsm_channels.h"
#include "tsm_playback.h" 

#include "ui/tsm_ui_template.h"
#include "ui/tsm_ui_registry.h"
#include "ui/tsm_ui_manager.h"
#include "ui/tsm_ui_main.h"
#include "ui/tsm_ui_announcement.h"

namespace fs = std::filesystem;

Uint32 pauseStartTime = 0;
Uint32 totalPausedTime = 0;

int main(int argc, char** argv)
{
    tsm::Logger::init();
    
    TSM::UI_Manager& uiManager = TSM::UI_Manager::GetInstance();
    if (!uiManager.Initialize()) {
        spdlog::error("Failed to initialize UI Manager");
        return -1;
    }

    uiManager.Run();
    uiManager.Shutdown();
    return 0;
}
