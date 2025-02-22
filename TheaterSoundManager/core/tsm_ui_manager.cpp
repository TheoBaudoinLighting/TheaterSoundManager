// tsm_ui_manager.cpp

#include "tsm_ui_manager.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <cmath>
#include <cstdio>
#include <ctime>        
#include <algorithm>    
#include <vector>
#include <spdlog/spdlog.h>

#include "tsm_audio_manager.h"
#include "tsm_playlist_manager.h"
#include "tsm_announcement_manager.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

namespace TSM
{

static char g_newMusicPath[256]       = "";
static char g_newAnnouncementPath[256]= "";
static char g_newSFXPath[256]         = "";

static char g_playlistName[256]       = "playlist_test"; 
static int  g_selectedMusicIndex      = -1; 
static int  g_selectedAnnouncement    = -1; 
static int  g_selectedSFX             = -1;  

static int  g_plannedHour            = 14; 
static int  g_plannedMinute          = 30; 
static char g_plannedAnnounceName[128]= "";

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h> 

std::vector<std::string> OpenFileDialogMultiSelect() {
    std::vector<std::string> filePaths;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
        return filePaths;

    IFileOpenDialog *pFileOpen;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, 
                         IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));

    if (SUCCEEDED(hr)) {
        DWORD dwFlags;
        pFileOpen->GetOptions(&dwFlags);
        pFileOpen->SetOptions(dwFlags | FOS_ALLOWMULTISELECT);

        COMDLG_FILTERSPEC fileTypes[] = {
            { L"Fichiers Audio", L"*.mp3;*.wav;*.ogg" },
            { L"Tous les fichiers", L"*.*" }
        };
        pFileOpen->SetFileTypes(2, fileTypes);

        hr = pFileOpen->Show(NULL);

        if (SUCCEEDED(hr)) {
            IShellItemArray *pItems;
            hr = pFileOpen->GetResults(&pItems);
            
            if (SUCCEEDED(hr)) {
                DWORD count;
                pItems->GetCount(&count);
                
                for(DWORD i = 0; i < count; i++) {
                    IShellItem *pItem;
                    hr = pItems->GetItemAt(i, &pItem);
                    if (SUCCEEDED(hr)) {
                        PWSTR filePath;
                        hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
                        if (SUCCEEDED(hr)) {
                            std::wstring ws(filePath);
                            std::string path(ws.begin(), ws.end());
                            filePaths.push_back(path);
                            CoTaskMemFree(filePath);
                        }
                        pItem->Release();
                    }
                }
                pItems->Release();
            }
        }
        pFileOpen->Release();
    }
    CoUninitialize();
    return filePaths;
}
#else
std::vector<std::string> OpenFileDialogMultiSelect() {
    return std::vector<std::string>();
}
#endif

void ImportAudioFiles() {
    auto filePaths = OpenFileDialogMultiSelect();
    
    // Ensure default playlist exists
    if (!PlaylistManager::GetInstance().GetPlaylistByName(g_playlistName)) {
        PlaylistManager::GetInstance().CreatePlaylist(g_playlistName);
    }
    
    for(const auto& path : filePaths) {
        std::string soundID;
        bool isMusic = false;
        
        std::string lowerPath = path;
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        
        if(lowerPath.find("music") != std::string::npos || 
           lowerPath.find("song") != std::string::npos) {
            soundID = "music_" + std::to_string(std::rand());
            isMusic = true;
        }
        else if(lowerPath.find("sfx") != std::string::npos) {
            soundID = "sfx_" + std::to_string(std::rand());
        }
        else if(lowerPath.find("announce") != std::string::npos) {
            soundID = "announce_" + std::to_string(std::rand());
        }
        else {
            soundID = "sound_" + std::to_string(std::rand());
            isMusic = true;
        }

        bool ok = AudioManager::GetInstance().LoadSound(soundID, path, isMusic);
        if(ok) {
            spdlog::info("Imported audio file '{}' as '{}'", path, soundID);
            
            if(isMusic) {
                PlaylistManager::GetInstance().AddToPlaylist(g_playlistName, soundID);
            }
        }
        else {
            spdlog::error("Failed to import audio file: {}", path);
        }
    }
}


UIManager::UIManager()
    : m_window(nullptr),
      m_glContext(nullptr),
      m_renderer(nullptr),
      m_isRunning(true),
      m_isInitialized(false),
      m_masterVolume(0.5f),
      m_musicVolume(0.5f),
      m_announcementVolume(0.5f),
      m_sfxVolume(0.5f),
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

    SetupBlenderStyle();

    io.ConfigDockingWithShift = true;

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 5.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 5.0f;

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 5.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        style.Colors[ImGuiCol_Border].w = 1.0f;
    }

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

    io.Fonts->AddFontFromFileTTF("assets/fonts/Jost-Regular.ttf", 24.0f);

    m_isInitialized = true;
    return true;
}

void UIManager::SetupBlenderStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        // Base colors - More pastel palette
        const ImVec4 blenderBackground = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
        const ImVec4 blenderPanel = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
        const ImVec4 blenderAccent = ImVec4(1.00f, 0.65f, 0.40f, 1.00f);  // Softer orange
        const ImVec4 blenderAccentBright = ImVec4(1.00f, 0.75f, 0.50f, 1.00f); // Brighter pastel orange
        const ImVec4 blenderAccentDark = ImVec4(0.85f, 0.55f, 0.30f, 1.00f);   // Darker pastel orange
        const ImVec4 blenderText = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);

        // Base elements
        colors[ImGuiCol_Text] = blenderText;
        colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        colors[ImGuiCol_WindowBg] = blenderBackground;
        colors[ImGuiCol_ChildBg] = blenderBackground;
        colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
        colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.10f, 0.10f, 0.10f, 0.00f);

        // Frame and interactive elements
        colors[ImGuiCol_FrameBg] = blenderPanel;
        colors[ImGuiCol_FrameBgHovered] = ImVec4(1.00f, 0.70f, 0.45f, 0.30f);
        colors[ImGuiCol_FrameBgActive] = blenderAccent;

        // Title bar
        colors[ImGuiCol_TitleBg] = blenderBackground;
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.15f, 0.90f);

        // Menu bar
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);

        // Scrollbar
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.80f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = blenderAccentDark;
        colors[ImGuiCol_ScrollbarGrabActive] = blenderAccent;

        // Interactive elements with pastel accents
        colors[ImGuiCol_CheckMark] = blenderAccentBright;
        colors[ImGuiCol_SliderGrab] = blenderAccent;
        colors[ImGuiCol_SliderGrabActive] = blenderAccentBright;
        colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = blenderAccent;
        colors[ImGuiCol_ButtonActive] = blenderAccentBright;

        // Headers
        colors[ImGuiCol_Header] = ImVec4(1.00f, 0.65f, 0.40f, 0.30f);
        colors[ImGuiCol_HeaderHovered] = blenderAccent;
        colors[ImGuiCol_HeaderActive] = blenderAccentBright;

        // Separator
        colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_SeparatorHovered] = blenderAccent;
        colors[ImGuiCol_SeparatorActive] = blenderAccentBright;

        // Resize grip
        colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 0.65f, 0.40f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered] = blenderAccent;
        colors[ImGuiCol_ResizeGripActive] = blenderAccentBright;

        // Tabs with pastel accents
        colors[ImGuiCol_Tab] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
        colors[ImGuiCol_TabHovered] = blenderAccent;
        colors[ImGuiCol_TabSelected] = ImVec4(1.00f, 0.65f, 0.40f, 0.40f);
        colors[ImGuiCol_TabSelectedOverline] = blenderAccentBright;
        colors[ImGuiCol_TabDimmed] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(1.00f, 0.65f, 0.40f, 0.25f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = blenderAccentDark;

        // Docking
        colors[ImGuiCol_DockingPreview] = ImVec4(1.00f, 0.65f, 0.40f, 0.50f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

        // Plot elements
        colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 0.65f, 0.40f, 0.80f);
        colors[ImGuiCol_PlotLinesHovered] = blenderAccentBright;
        colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 0.65f, 0.40f, 0.80f);
        colors[ImGuiCol_PlotHistogramHovered] = blenderAccentBright;

        // Tables
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);

        // Text and selection
        colors[ImGuiCol_TextLink] = ImVec4(1.00f, 0.70f, 0.45f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(1.00f, 0.65f, 0.40f, 0.35f);

        // Navigation and modal
        colors[ImGuiCol_DragDropTarget] = blenderAccentBright;
        colors[ImGuiCol_NavCursor] = blenderAccent;
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 0.65f, 0.40f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.50f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.40f);

        // Style configuration
        style.WindowRounding = 3.0f;
        style.ChildRounding = 3.0f;
        style.FrameRounding = 3.0f;
        style.PopupRounding = 3.0f;
        style.ScrollbarRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;

        // Spacing and alignment
        style.FramePadding = ImVec2(4, 3);
        style.ItemSpacing = ImVec2(8, 4);
        style.ItemInnerSpacing = ImVec2(4, 4);
        style.CellPadding = ImVec2(4, 2);
        style.TouchExtraPadding = ImVec2(0, 0);

        // Border and window settings
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
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

    RenderAudioLibrary();

    ImGui::Begin("Announcements");
    RenderAnnouncementControls();
    ImGui::End();

    ImGui::Begin("Debug Info");
    RenderDebugInfo();
    ImGui::End();

    ImGui::Begin("Volume Controls");
    RenderAudioControls();
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

void UIManager::RenderAudioLibrary() {
    ImGui::Begin("Audio Library Manager");
    if (ImGui::BeginTabBar("AudioLibraryTabs")) {
        if (ImGui::BeginTabItem("Music / Playlists")) {
            RenderMusicPlaylistTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Announcements")) {
            RenderAnnouncementsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SFX")) {
            RenderSFXTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void UIManager::EnsureDefaultPlaylist() {
    if (!PlaylistManager::GetInstance().GetPlaylistByName(g_playlistName)) {
        PlaylistManager::GetInstance().CreatePlaylist(g_playlistName);
    }
}

std::string UIManager::GetDisplayName(const std::string& path) const {
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        return path.substr(lastSlash + 1);
    }
    return path;
}

void UIManager::RenderMusicPlaylistTab() 
{
    if (ImGui::Button("Import Music Files", ImVec2(200, 30))) {
        EnsureDefaultPlaylist();
        ImportAudioFiles();
    }

    ImGui::Spacing();
    ImGui::Separator();
    
    auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(g_playlistName);
    if (!playlist) return;

    if (ImGui::BeginTable("MusicTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("File Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 250.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < playlist->tracks.size(); i++) {
            const auto& trackId = playlist->tracks[i];

            std::string duration = "Unknown"; 
            std::string fileName = "Unknown"; 

            auto soundIt = AudioManager::GetInstance().GetAllSounds().find(trackId);
            if (soundIt != AudioManager::GetInstance().GetAllSounds().end() && soundIt->second.sound) {
                unsigned int lengthMs = 0;
                if (soundIt->second.sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) == FMOD_OK) {
                    int minutes = (lengthMs / 1000) / 60;
                    int seconds = (lengthMs / 1000) % 60;
                    char buffer[32];
                    snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, seconds);
                    duration = buffer;
                }

                fileName = GetDisplayName(soundIt->second.filePath);
            }

            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool selected = (g_selectedMusicIndex == static_cast<int>(i));
            if (ImGui::RadioButton("##sel", selected)) {
                g_selectedMusicIndex = static_cast<int>(i);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", fileName.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", duration.c_str());

            ImGui::TableNextColumn();
            if (ImGui::Button("Play")) {
                PlaylistManager::GetInstance().PlayFromIndex(g_playlistName, i);
            }
            ImGui::SameLine();
            
            if (i > 0) {
                if (ImGui::Button("Up")) {
                    PlaylistManager::GetInstance().MoveTrackUp(g_playlistName, i);
                    if (g_selectedMusicIndex == static_cast<int>(i)) {
                        g_selectedMusicIndex--;
                    }
                }
                ImGui::SameLine();
            }
            
            if (i < playlist->tracks.size() - 1) {
                if (ImGui::Button("Down")) {
                    PlaylistManager::GetInstance().MoveTrackDown(g_playlistName, i);
                    if (g_selectedMusicIndex == static_cast<int>(i)) {
                        g_selectedMusicIndex++;
                    }
                }
                ImGui::SameLine();
            }

            if (ImGui::Button("Delete")) {
                PlaylistManager::GetInstance().RemoveFromPlaylist(g_playlistName, trackId);
                g_selectedMusicIndex = -1;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void UIManager::RenderAnnouncementsTab() {
    if (ImGui::Button("Import Announcement", ImVec2(200, 30))) {
        ImportAudioFiles();
    }
    ImGui::Separator();

    if (ImGui::BeginTable("AnnouncementTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("File Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableHeadersRow();

        std::vector<std::string> announcements;
        const auto allSounds = AudioManager::GetInstance().GetAllSounds();
        for (const auto& [soundId, soundData] : allSounds) {
            if (soundId.find("announce") != std::string::npos) {
                announcements.push_back(soundId);
            }
        }
        
        for (size_t i = 0; i < announcements.size(); i++) {
            const auto& soundId = announcements[i];
            
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            bool selected = (g_selectedAnnouncement == static_cast<int>(i));
            if (ImGui::RadioButton("##sel", selected)) {
                g_selectedAnnouncement = static_cast<int>(i);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", GetDisplayName(soundId).c_str());

            ImGui::TableNextColumn();
            if (ImGui::Button("Play")) {
                AudioManager::GetInstance().PlaySound(soundId, false, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                AudioManager::GetInstance().StopSound(soundId);
                g_selectedAnnouncement = -1;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void UIManager::RenderSFXTab() {
    if (ImGui::Button("Import SFX", ImVec2(200, 30))) {
        ImportAudioFiles();
    }
    ImGui::Separator();

    if (ImGui::BeginTable("SFXTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("File Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableHeadersRow();

        std::vector<std::string> sfxList;
        const auto& allSounds = AudioManager::GetInstance().GetAllSounds();
        for (const auto& kv : allSounds) {
            if (kv.first.find("sfx") != std::string::npos) {
                sfxList.push_back(kv.first);
            }
        }

        for (size_t i = 0; i < sfxList.size(); i++) {
            const auto& sfxId = sfxList[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            bool selected = (g_selectedSFX == static_cast<int>(i));
            if (ImGui::RadioButton("##sel", selected)) {
                g_selectedSFX = static_cast<int>(i);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", GetDisplayName(sfxId).c_str());

            ImGui::TableNextColumn();
            if (ImGui::Button("Play")) {
                AudioManager::GetInstance().PlaySound(sfxId, false, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                AudioManager::GetInstance().StopSound(sfxId);
                g_selectedSFX = -1;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
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
    std::string duration = "Unknown";
    std::string fileName = "Unknown";
    if (!currentTrack.empty()) 
    {
        auto soundIt = AudioManager::GetInstance().GetAllSounds().find(currentTrack);
        if (soundIt != AudioManager::GetInstance().GetAllSounds().end() && soundIt->second.sound) {
            unsigned int lengthMs = 0;
            if (soundIt->second.sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) == FMOD_OK) {
                int minutes = (lengthMs / 1000) / 60;
                int seconds = (lengthMs / 1000) % 60;
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, seconds);
                duration = buffer;
            }

            fileName = GetDisplayName(soundIt->second.filePath);
        }
        
        ImGui::Spacing();
        ImGui::Text("Now Playing: %s", fileName.c_str());

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