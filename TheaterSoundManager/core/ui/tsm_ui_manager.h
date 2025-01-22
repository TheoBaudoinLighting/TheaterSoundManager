// tsm_ui_manager.h

#pragma once

#include <SDL.h>
#include <SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_sdl2.h>

#include "tsm_playback.h"
#include "tsm_announcement.h"

#include <string>

namespace TSM {

class UI_Manager 
{
public:
    static UI_Manager& GetInstance() {
        static UI_Manager instance;
        return instance;
    }

    UI_Manager(const UI_Manager&) = delete;
    UI_Manager& operator=(const UI_Manager&) = delete;
    UI_Manager(UI_Manager&&) = delete;
    UI_Manager& operator=(UI_Manager&&) = delete;

    bool Initialize(const std::string& title = "Theater Sound Manager", int width = 1280, int height = 720);
    void Shutdown();
    void Run();

    void ProcessEvents();
    void PreRender();
    void Render();
    void PostRender();

    void SetupMayaStyle();
    void SetupBlenderStyle();
    void SetupHoudiniStyle();

    bool IsRunning() const { return m_isRunning; }
    PlaybackManager* GetPlaybackManager() const { return m_playbackManager; }
    AnnouncementManager* GetAnnouncementManager() const { return m_announcementManager; }
    FMOD::System* GetFmodSystem() const { return m_fmodSystem; }

private:
    UI_Manager();
    ~UI_Manager();
    
    void SetupDockSpace();

    SDL_Window* m_window = nullptr;
    SDL_GLContext m_gl_context = nullptr;

    FMOD::System* m_fmodSystem = nullptr;
    FMOD_RESULT m_fmodResult = FMOD_OK;

    PlaybackManager* m_playbackManager = nullptr;
    AnnouncementManager* m_announcementManager = nullptr;

    bool m_isRunning = false;
};

}
