// tsm_ui_manager.cpp

#include "tsm_ui_manager.h"

#include <cmath>
#include <cstdio>

#include <SDL.h>
#include <SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#include <spdlog/spdlog.h>

#include "tsm_playlist_manager.h"
#include "tsm_announcement_manager.h"
#include "tsm_audio_manager.h"

namespace TSM
{

UIManager::UIManager()
    : m_window(nullptr),
      m_glContext(nullptr),
      m_renderer(nullptr),
      m_isRunning(true),
      m_isInitialized(false),
      m_masterVolume(0.05f),
      m_musicVolume(0.05f),
      m_announcementVolume(0.05f),
      m_sfxVolume(0.05f),
      m_duckFactor(1.0f) 
{
}

bool UIManager::Init(int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        spdlog::error("SDL initialization failed: {}", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    m_window = SDL_CreateWindow(
        "TSM Audio Manager",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        window_flags
    );

    if (!m_window) {
        spdlog::error("Window creation failed: {}", SDL_GetError());
        return false;
    }
    
    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        spdlog::error("OpenGL context creation failed: {}", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(m_window, m_glContext);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;    
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  
    io.ConfigWindowsMoveFromTitleBarOnly = true;         

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplSDL2_InitForOpenGL(m_window, m_glContext)) {
        spdlog::error("ImGui SDL2 initialization failed");
        return false;
    }

    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        spdlog::error("ImGui OpenGL initialization failed");
        return false;
    }

    m_isInitialized = true;
    return true;
}

void UIManager::Shutdown()
{
    if (!m_isInitialized) return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }
    
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
    m_isInitialized = false;
}

bool UIManager::HandleEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        
        if (event.type == SDL_QUIT) {
            m_isRunning = false;
        }
        else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_CLOSE && 
                event.window.windowID == SDL_GetWindowID(m_window)) {
                m_isRunning = false;
            }
        }
    }
    return m_isRunning;
}

void UIManager::PreRender()
{
    if (!m_isInitialized) return;

    if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    
    ImGui::Begin("DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("Options"))
        {
            if (ImGui::MenuItem("Exit")) { m_isRunning = false; }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

void UIManager::Render()
{
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("Theater Sound Manager", nullptr, ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Exit")) {
                m_isRunning = false;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();

    ImGui::Begin("Playlist Controls");
    RenderPlaylistControls();
    ImGui::End();

    ImGui::Begin("Announcements");
    RenderAnnouncementControls();
    ImGui::End();

    ImGui::Begin("Volume Controls");
    RenderAudioControls();
    ImGui::End();

    ImGui::Begin("Debug Info");
    RenderDebugInfo();
    ImGui::End();
}

void UIManager::PostRender()
{
    if (!m_isInitialized) return;

    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    
    ImVec4 clear_color = ImVec4(0.1f, 0.105f, 0.11f, 1.00f);
    glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
    }

    SDL_GL_SwapWindow(m_window);
}

void UIManager::RenderPlaylistControls()
{
    static char playlistName[256] = "playlist_test";
    static PlaylistOptions opts;
    static float crossfadeDuration = 5.0f;
    
    ImGui::Text("Playlist Controls");
    ImGui::Separator();

    ImGui::InputText("Playlist Name", playlistName, IM_ARRAYSIZE(playlistName));

    if (ImGui::Button("Create Playlist")) {
        PlaylistManager::GetInstance().CreatePlaylist(playlistName);
    }

    ImGui::SameLine();
    
    if (ImGui::Button("Play")) {
        PlaylistManager::GetInstance().Play(playlistName, opts);
        UpdateAllVolumes();
    }

    ImGui::SameLine();

    if (ImGui::Button("Stop")) {
        PlaylistManager::GetInstance().Stop(playlistName);
    }

    ImGui::Checkbox("Random Order", &opts.randomOrder);
    ImGui::Checkbox("Random Segment", &opts.randomSegment);
    ImGui::Checkbox("Loop Playlist", &opts.loopPlaylist);
    ImGui::SliderFloat("Segment Duration", &opts.segmentDuration, 10.0f, 300.0f, "%.1f s");
    
    if (ImGui::SliderFloat("Crossfade Duration", &crossfadeDuration, 0.0f, 10.0f, "%.1f s")) {
        PlaylistManager::GetInstance().SetCrossfadeDuration(crossfadeDuration);
    }
    
    ImGui::Spacing();
    ImGui::Separator();
    
    if (ImGui::SliderFloat("Music Volume", &m_musicVolume, 0.0f, 1.0f)) {
        UpdateAllVolumes();
    }

    std::string currentTrack = PlaylistManager::GetInstance().GetCurrentTrackName();
    if (!currentTrack.empty()) 
    {
        size_t lastSlash = currentTrack.find_last_of("/\\");
        std::string displayName = (lastSlash == std::string::npos) ? 
            currentTrack : currentTrack.substr(lastSlash + 1);

        ImGui::Spacing();
        ImGui::Text("Now Playing: %s", displayName.c_str());

        auto& playlist = PlaylistManager::GetInstance();
        FMOD::Channel* currentChannel = playlist.GetCurrentChannel();
        bool isInCrossfade = playlist.IsInCrossfade();
        float crossfadeProgress = playlist.GetCrossfadeProgress();
        
        if (currentChannel)
        {
            FMOD::Sound* currentSound = nullptr;
            unsigned int positionMs = 0;
            unsigned int lengthMs = 0;
            bool isPlaying = false;

            currentChannel->isPlaying(&isPlaying);
            
            if (isPlaying && 
                currentChannel->getCurrentSound(&currentSound) == FMOD_OK && 
                currentSound)
            {
                currentSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
                currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);

                float totalMinutes = floorf((lengthMs / 1000.0f) / 60.0f);
                float totalSeconds = fmodf((lengthMs / 1000.0f), 60.0f);
                ImGui::Text("Total Duration: %.0f:%.02f", totalMinutes, totalSeconds);

                float progress = static_cast<float>(positionMs) / static_cast<float>(lengthMs);
                
                float remainingTimeMs = lengthMs - positionMs;
                float remainingMinutes = floorf((remainingTimeMs / 1000.0f) / 60.0f);
                float remainingSeconds = fmodf((remainingTimeMs / 1000.0f), 60.0f);
                
                char progressText[32];
                snprintf(progressText, sizeof(progressText), "%.0f:%.02f remaining", 
                        remainingMinutes, remainingSeconds);

                if (isInCrossfade)
                {
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, 
                        ImVec4(0.7f, 0.7f, 1.0f, 1.0f - crossfadeProgress));
                }

                ImGui::ProgressBar(progress, ImVec2(-1, 0), progressText);

                if (isInCrossfade)
                {
                    ImGui::PopStyleColor();
                }
            }
        }

        if (PlaylistManager::GetInstance().IsInCrossfade())
        {
            float crossfadeProgress = PlaylistManager::GetInstance().GetCrossfadeProgress();
            
            char crossfadeText[32];
            snprintf(crossfadeText, sizeof(crossfadeText), "Crossfade: %.1f%%", crossfadeProgress * 100.0f);
            
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.2f, 0.2f, 0.8f));
            ImGui::ProgressBar(crossfadeProgress, ImVec2(-1, 0), crossfadeText);
            ImGui::PopStyleColor();
        }

        if (opts.randomSegment)
        {
            float segmentProgress = PlaylistManager::GetInstance().GetSegmentProgress();
            float remainingSegmentTime = opts.segmentDuration * (1.0f - segmentProgress);
            
            char segmentText[32];
            snprintf(segmentText, sizeof(segmentText), "%.1fs remaining", remainingSegmentTime);
            
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            ImGui::ProgressBar(segmentProgress, ImVec2(-1, 0), segmentText);
            ImGui::PopStyleColor();
        }
    }
}

void UIManager::RenderAnnouncementControls()
{
    static char announceName[256] = "announce1";
    static float duckVolume = 0.3f;

    static bool useSFXBefore = true;
    static bool useSFXAfter  = true;

    ImGui::Text("Announcement Controls");
    ImGui::Separator();

    ImGui::InputText("Announcement Name", announceName, IM_ARRAYSIZE(announceName));
    ImGui::SliderFloat("Duck Volume", &duckVolume, 0.0f, 1.0f);

    ImGui::Checkbox("Use SFX Before", &useSFXBefore);
    ImGui::Checkbox("Use SFX After",  &useSFXAfter);

    if (ImGui::Button("Play Announcement")) {
        AnnouncementManager::GetInstance().PlayAnnouncement(announceName, duckVolume, useSFXBefore, useSFXAfter);
    }

    ImGui::SameLine();

    if (ImGui::Button("Stop Announcement")) {
        AnnouncementManager::GetInstance().StopAnnouncement();
    }
}

void UIManager::RenderAudioControls()
{
    ImGui::Text("Volume Controls");
    ImGui::Separator();

    if (ImGui::SliderFloat("Master Volume", &m_masterVolume, 0.0f, 1.0f)) {
        UpdateAllVolumes();
    }

    ImGui::Spacing();

    if (ImGui::SliderFloat("Music Volume", &m_musicVolume, 0.0f, 1.0f)) {
        UpdateAllVolumes();
    }

    if (ImGui::SliderFloat("Announcement Volume", &m_announcementVolume, 0.0f, 1.0f)) {
        UpdateAllVolumes();
    }

    if (ImGui::SliderFloat("SFX Volume", &m_sfxVolume, 0.0f, 1.0f)) {
        UpdateAllVolumes();
    }
}

void UIManager::RenderDebugInfo()
{
    ImGui::Text("Debug Information");
    ImGui::Separator();

    float frameRate = ImGui::GetIO().Framerate;
    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / frameRate, frameRate);
}

void UIManager::UpdateAllVolumes()
{
    for (const auto& kv : AudioManager::GetInstance().GetAllSounds()) 
    {
        const std::string& soundName = kv.first; 
        const auto& soundData = kv.second;

        float baseVolume = 1.0f;

        bool isAnnouncement = (soundName.find("announce") != std::string::npos);
        bool isSfx          = (soundName.find("sfx") != std::string::npos);

        if (isAnnouncement) {
            baseVolume = m_announcementVolume;
        }
        else if (isSfx) {
            baseVolume = m_sfxVolume;
        }
        else {
            baseVolume = m_musicVolume * m_duckFactor;
        }

        float finalVolume = m_masterVolume * baseVolume;

        for (auto* channel : soundData.channels) {
            if (!channel) continue;
            bool isPlaying = false;
            channel->isPlaying(&isPlaying);

            if (isPlaying) {
                channel->setVolume(finalVolume);
            }
        }
    }
}


}