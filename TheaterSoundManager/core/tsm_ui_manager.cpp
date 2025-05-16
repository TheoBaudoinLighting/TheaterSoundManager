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

std::vector<std::string> OpenAnnouncementFileDialog() {
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

        pFileOpen->SetTitle(L"Sélectionner des fichiers d'annonce");

        COMDLG_FILTERSPEC fileTypes[] = {
            { L"Fichiers Audio d'Annonce", L"*.mp3;*.wav;*.ogg" },
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

std::vector<std::string> OpenAnnouncementFileDialog() {
    return std::vector<std::string>();
}
#endif

void ImportAnnouncementFiles() {
    auto filePaths = OpenAnnouncementFileDialog();
    
    for(const auto& path : filePaths) {
        std::string fileName = path;
        size_t lastSlash = fileName.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            fileName = fileName.substr(lastSlash + 1);
        }
        
        size_t lastDot = fileName.find_last_of(".");
        if (lastDot != std::string::npos) {
            fileName = fileName.substr(0, lastDot);
        }
        
        std::string announceID = "announce_" + fileName;
        
        bool ok = AnnouncementManager::GetInstance().LoadAnnouncement(announceID, path);
        if(ok) {
            spdlog::info("Imported announcement file '{}' as '{}'", path, announceID);
        }
        else {
            spdlog::error("Failed to import announcement file: {}", path);
        }
    }
}

void ImportAudioFiles() {
    auto filePaths = OpenFileDialogMultiSelect();
    
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
      m_announcementVolume(3.0f),
      m_sfxVolume(3.0f),
      m_duckFactor(1.0f),
      m_weddingModeActive(false),
      m_weddingPhase(0),
      m_autoDuckingActive(false),
      m_originalDuckFactor(1.0f),
      m_targetDuckFactor(0.3f),
      m_crossfadeDuration(5.0f),
      m_autoTransitionToPhase2(false),
      m_transitionToNormalMusicAfterWedding(false),
      m_weddingEntranceSoundId("wedding_entrance_sound"),
      m_weddingCeremonySoundId("wedding_ceremony_sound"),
      m_weddingExitSoundId("wedding_exit_sound"),
      m_normalPlaylistAfterWedding("playlist_test")
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

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED);
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
    
    UpdateWeddingFilePaths();
    
    return true;
}

void UIManager::SetupBlenderStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        const ImVec4 blenderBackground = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
        const ImVec4 blenderPanel = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
        const ImVec4 blenderAccent = ImVec4(1.00f, 0.65f, 0.40f, 1.00f);  
        const ImVec4 blenderAccentBright = ImVec4(1.00f, 0.75f, 0.50f, 1.00f); 
        const ImVec4 blenderAccentDark = ImVec4(0.85f, 0.55f, 0.30f, 1.00f); 
        const ImVec4 blenderText = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);

        colors[ImGuiCol_Text] = blenderText;
        colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
        colors[ImGuiCol_WindowBg] = blenderBackground;
        colors[ImGuiCol_ChildBg] = blenderBackground;
        colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.98f);
        colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.10f, 0.10f, 0.10f, 0.00f);

        colors[ImGuiCol_FrameBg] = blenderPanel;
        colors[ImGuiCol_FrameBgHovered] = ImVec4(1.00f, 0.70f, 0.45f, 0.30f);
        colors[ImGuiCol_FrameBgActive] = blenderAccent;

        colors[ImGuiCol_TitleBg] = blenderBackground;
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.15f, 0.90f);

        colors[ImGuiCol_MenuBarBg] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);

        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.80f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = blenderAccentDark;
        colors[ImGuiCol_ScrollbarGrabActive] = blenderAccent;

        colors[ImGuiCol_CheckMark] = blenderAccentBright;
        colors[ImGuiCol_SliderGrab] = blenderAccent;
        colors[ImGuiCol_SliderGrabActive] = blenderAccentBright;
        colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = blenderAccent;
        colors[ImGuiCol_ButtonActive] = blenderAccentBright;

        colors[ImGuiCol_Header] = ImVec4(1.00f, 0.65f, 0.40f, 0.30f);
        colors[ImGuiCol_HeaderHovered] = blenderAccent;
        colors[ImGuiCol_HeaderActive] = blenderAccentBright;

        colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_SeparatorHovered] = blenderAccent;
        colors[ImGuiCol_SeparatorActive] = blenderAccentBright;

        colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 0.65f, 0.40f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered] = blenderAccent;
        colors[ImGuiCol_ResizeGripActive] = blenderAccentBright;

        colors[ImGuiCol_Tab] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
        colors[ImGuiCol_TabHovered] = blenderAccent;
        colors[ImGuiCol_TabSelected] = ImVec4(1.00f, 0.65f, 0.40f, 0.40f);
        colors[ImGuiCol_TabSelectedOverline] = blenderAccentBright;
        colors[ImGuiCol_TabDimmed] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(1.00f, 0.65f, 0.40f, 0.25f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = blenderAccentDark;

        colors[ImGuiCol_DockingPreview] = ImVec4(1.00f, 0.65f, 0.40f, 0.50f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

        colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 0.65f, 0.40f, 0.80f);
        colors[ImGuiCol_PlotLinesHovered] = blenderAccentBright;
        colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 0.65f, 0.40f, 0.80f);
        colors[ImGuiCol_PlotHistogramHovered] = blenderAccentBright;

        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);

        colors[ImGuiCol_TextLink] = ImVec4(1.00f, 0.70f, 0.45f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(1.00f, 0.65f, 0.40f, 0.35f);

        colors[ImGuiCol_DragDropTarget] = blenderAccentBright;
        colors[ImGuiCol_NavCursor] = blenderAccent;
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 0.65f, 0.40f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.50f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.40f);

        style.WindowRounding = 3.0f;
        style.ChildRounding = 3.0f;
        style.FrameRounding = 3.0f;
        style.PopupRounding = 3.0f;
        style.ScrollbarRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;

        style.FramePadding = ImVec2(4, 3);
        style.ItemSpacing = ImVec2(8, 4);
        style.ItemInnerSpacing = ImVec2(4, 4);
        style.CellPadding = ImVec2(4, 2);
        style.TouchExtraPadding = ImVec2(0, 0);

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

void UIManager::RenderPlaylistManagerTab()
{
    static char newPlaylistName[256] = "";
    static int selectedPlaylistIndex = -1;
    static std::vector<std::string> playlistNames;
    static char renameBuffer[256] = "";
    static bool renameMode = false;
    static bool showImportExportOptions = false;
    static char importExportPath[512] = "playlists.json";
    
    if (ImGui::Button("Actualiser les playlists", ImVec2(200, 30)))
    {
        playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
        if (selectedPlaylistIndex >= static_cast<int>(playlistNames.size()))
        {
            selectedPlaylistIndex = -1;
        }
    }
    
    // Section création de playlist
    ImGui::Separator();
    ImGui::Text("Créer une nouvelle playlist");
    
    ImGui::InputText("Nom de la playlist", newPlaylistName, IM_ARRAYSIZE(newPlaylistName));
    
    if (ImGui::Button("Créer", ImVec2(100, 30)))
    {
        if (strlen(newPlaylistName) > 0)
        {
            PlaylistManager::GetInstance().CreatePlaylist(newPlaylistName);
            playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
            newPlaylistName[0] = '\0';
        }
    }
    
    // Section liste des playlists
    ImGui::Separator();
    ImGui::Text("Playlists disponibles");
    
    if (playlistNames.empty())
    {
        playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
    }
    
    if (ImGui::BeginTable("PlaylistsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Pistes", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Statut", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableHeadersRow();
        
        for (int i = 0; i < playlistNames.size(); i++)
        {
            const auto& playlistName = playlistNames[i];
            
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            
            bool selected = (selectedPlaylistIndex == i);
            if (ImGui::Selectable(playlistName.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
            {
                selectedPlaylistIndex = i;
                if (!renameMode)
                {
                    strncpy(renameBuffer, playlistName.c_str(), IM_ARRAYSIZE(renameBuffer) - 1);
                    renameBuffer[IM_ARRAYSIZE(renameBuffer) - 1] = '\0';
                }
            }
            
            ImGui::TableNextColumn();
            int trackCount = static_cast<int>(PlaylistManager::GetInstance().GetPlaylistTrackCount(playlistName));
            ImGui::Text("%d", trackCount);
            
            ImGui::TableNextColumn();
            bool isPlaying = PlaylistManager::GetInstance().IsPlaylistPlaying(playlistName);
            ImGui::TextColored(isPlaying ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                            isPlaying ? "En lecture" : "Arrêtée");
            
            ImGui::TableNextColumn();
            if (ImGui::Button("Lecture", ImVec2(70, 25)))
            {
                PlaylistOptions opts;
                opts.randomOrder = false;
                opts.loopPlaylist = true;
                PlaylistManager::GetInstance().Play(playlistName, opts);
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Arrêter", ImVec2(70, 25)))
            {
                PlaylistManager::GetInstance().Stop(playlistName);
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Renommer", ImVec2(80, 25)))
            {
                renameMode = true;
                selectedPlaylistIndex = i;
                strncpy(renameBuffer, playlistName.c_str(), IM_ARRAYSIZE(renameBuffer) - 1);
                renameBuffer[IM_ARRAYSIZE(renameBuffer) - 1] = '\0';
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Supprimer", ImVec2(80, 25)))
            {
                ImGui::OpenPopup("Confirmer suppression");
            }
            
            if (ImGui::BeginPopupModal("Confirmer suppression", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Êtes-vous sûr de vouloir supprimer la playlist '%s'?", playlistName.c_str());
                ImGui::Text("Cette action est irréversible!");
                
                if (ImGui::Button("Confirmer", ImVec2(120, 30)))
                {
                    PlaylistManager::GetInstance().DeletePlaylist(playlistName);
                    playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
                    if (selectedPlaylistIndex == i)
                    {
                        selectedPlaylistIndex = -1;
                    }
                    ImGui::CloseCurrentPopup();
                }
                
                ImGui::SameLine();
                
                if (ImGui::Button("Annuler", ImVec2(120, 30)))
                {
                    ImGui::CloseCurrentPopup();
                }
                
                ImGui::EndPopup();
            }
            
            ImGui::PopID();
        }
        
        ImGui::EndTable();
    }
    
    // Section renommage de playlist
    if (renameMode && selectedPlaylistIndex >= 0 && selectedPlaylistIndex < playlistNames.size())
    {
        ImGui::Separator();
        ImGui::Text("Renommer la playlist '%s'", playlistNames[selectedPlaylistIndex].c_str());
        
        ImGui::InputText("Nouveau nom", renameBuffer, IM_ARRAYSIZE(renameBuffer));
        
        if (ImGui::Button("Appliquer", ImVec2(100, 30)))
        {
            if (strlen(renameBuffer) > 0)
            {
                PlaylistManager::GetInstance().RenamePlaylist(
                    playlistNames[selectedPlaylistIndex], renameBuffer);
                playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
                renameMode = false;
            }
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Annuler", ImVec2(100, 30)))
        {
            renameMode = false;
        }
    }
    
    // Section détail de la playlist sélectionnée
    if (selectedPlaylistIndex >= 0 && selectedPlaylistIndex < playlistNames.size())
    {
        const std::string& selectedPlaylist = playlistNames[selectedPlaylistIndex];
        const auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(selectedPlaylist);
        
        if (playlist)
        {
            ImGui::Separator();
            ImGui::Text("Détails de la playlist: %s", selectedPlaylist.c_str());
            
            // Options de lecture
            ImGui::Checkbox("Lecture aléatoire", const_cast<bool*>(&playlist->options.randomOrder));
            ImGui::SameLine();
            ImGui::Checkbox("Segment aléatoire", const_cast<bool*>(&playlist->options.randomSegment));
            ImGui::SameLine();
            ImGui::Checkbox("Boucler la playlist", const_cast<bool*>(&playlist->options.loopPlaylist));
            ImGui::SliderFloat("Durée du segment", const_cast<float*>(&playlist->options.segmentDuration), 10.0f, 300.0f, "%.1fs");
            
            // Liste des pistes
            ImGui::Text("Pistes dans la playlist:");
            
            if (ImGui::BeginTable("TracksTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
                ImGui::TableHeadersRow();
                
                for (size_t i = 0; i < playlist->tracks.size(); i++)
                {
                    const auto& trackId = playlist->tracks[i];
                    
                    ImGui::TableNextRow();
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", static_cast<int>(i));
                    
                    ImGui::TableNextColumn();
                    
                    std::string trackName = "Unknown";
                    auto soundIt = AudioManager::GetInstance().GetAllSounds().find(trackId);
                    if (soundIt != AudioManager::GetInstance().GetAllSounds().end())
                    {
                        trackName = GetDisplayName(soundIt->second.filePath);
                    }
                    
                    ImGui::Text("%s", trackName.c_str());
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", trackId.c_str());
                    
                    ImGui::TableNextColumn();
                    
                    ImGui::PushID(static_cast<int>(i) + 10000);
                    
                    if (ImGui::Button("Jouer", ImVec2(50, 25)))
                    {
                        PlaylistManager::GetInstance().PlayFromIndex(selectedPlaylist, static_cast<int>(i));
                    }
                    
                    ImGui::SameLine();
                    
                    if (i > 0)
                    {
                        if (ImGui::Button("↑", ImVec2(25, 25)))
                        {
                            PlaylistManager::GetInstance().MoveTrackUp(selectedPlaylist, static_cast<int>(i));
                        }
                        
                        ImGui::SameLine();
                    }
                    else
                    {
                        ImGui::Dummy(ImVec2(25, 25));
                        ImGui::SameLine();
                    }
                    
                    if (i < playlist->tracks.size() - 1)
                    {
                        if (ImGui::Button("↓", ImVec2(25, 25)))
                        {
                            PlaylistManager::GetInstance().MoveTrackDown(selectedPlaylist, static_cast<int>(i));
                        }
                        
                        ImGui::SameLine();
                    }
                    else
                    {
                        ImGui::Dummy(ImVec2(25, 25));
                        ImGui::SameLine();
                    }
                    
                    if (ImGui::Button("Supprimer", ImVec2(70, 25)))
                    {
                        PlaylistManager::GetInstance().RemoveFromPlaylistAtIndex(selectedPlaylist, i);
                    }
                    
                    ImGui::PopID();
                }
                
                ImGui::EndTable();
            }
            
            // Ajouter de nouvelles pistes
            static int selectedTrackToAdd = -1;
            static std::vector<std::pair<std::string, std::string>> availableTracks;
            
            if (ImGui::Button("Rafraîchir les pistes disponibles", ImVec2(250, 30)))
            {
                availableTracks.clear();
                
                for (const auto& [soundId, soundData] : AudioManager::GetInstance().GetAllSounds())
                {
                    if (soundId.find("sfx_") == std::string::npos && 
                        soundId.find("announce_") == std::string::npos)
                    {
                        availableTracks.push_back({soundId, GetDisplayName(soundData.filePath)});
                    }
                }
                
                std::sort(availableTracks.begin(), availableTracks.end(), 
                    [](const auto& a, const auto& b) { return a.second < b.second; });
            }
            
            if (availableTracks.empty())
            {
                if (ImGui::Button("Charger les pistes disponibles", ImVec2(250, 30)))
                {
                    availableTracks.clear();
                    
                    for (const auto& [soundId, soundData] : AudioManager::GetInstance().GetAllSounds())
                    {
                        if (soundId.find("sfx_") == std::string::npos && 
                            soundId.find("announce_") == std::string::npos)
                        {
                            availableTracks.push_back({soundId, GetDisplayName(soundData.filePath)});
                        }
                    }
                    
                    std::sort(availableTracks.begin(), availableTracks.end(), 
                        [](const auto& a, const auto& b) { return a.second < b.second; });
                }
            }
            else
            {
                ImGui::Text("Ajouter une piste à la playlist:");
                
                if (ImGui::BeginListBox("##TracksToAdd", ImVec2(-1, 200)))
                {
                    for (int i = 0; i < availableTracks.size(); i++)
                    {
                        const bool isSelected = (selectedTrackToAdd == i);
                        std::string displayName = availableTracks[i].second + " (" + availableTracks[i].first + ")";
                        
                        if (ImGui::Selectable(displayName.c_str(), isSelected))
                        {
                            selectedTrackToAdd = i;
                        }
                        
                        if (isSelected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndListBox();
                }
                
                if (selectedTrackToAdd >= 0 && selectedTrackToAdd < availableTracks.size())
                {
                    if (ImGui::Button("Ajouter à la playlist", ImVec2(200, 30)))
                    {
                        PlaylistManager::GetInstance().AddToPlaylist(
                            selectedPlaylist, availableTracks[selectedTrackToAdd].first);
                        selectedTrackToAdd = -1;
                    }
                }
            }
        }
    }
    
    // Import/Export options
    ImGui::Separator();
    
    if (ImGui::CollapsingHeader("Options d'importation/exportation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputText("Chemin du fichier", importExportPath, IM_ARRAYSIZE(importExportPath));
        
        if (ImGui::Button("Importer toutes les playlists", ImVec2(250, 30)))
        {
            if (PlaylistManager::GetInstance().LoadPlaylistsFromFile(importExportPath))
            {
                playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
                selectedPlaylistIndex = -1;
            }
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Exporter toutes les playlists", ImVec2(250, 30)))
        {
            PlaylistManager::GetInstance().SavePlaylistsToFile(importExportPath);
        }
        
        if (selectedPlaylistIndex >= 0 && selectedPlaylistIndex < playlistNames.size())
        {
            const std::string& selectedPlaylist = playlistNames[selectedPlaylistIndex];
            
            ImGui::Separator();
            ImGui::Text("Import/Export pour la playlist sélectionnée: %s", selectedPlaylist.c_str());
            
            static char singlePlaylistPath[512] = "playlist_single.json";
            ImGui::InputText("Chemin du fichier unique", singlePlaylistPath, IM_ARRAYSIZE(singlePlaylistPath));
            
            if (ImGui::Button("Exporter cette playlist", ImVec2(200, 30)))
            {
                PlaylistManager::GetInstance().ExportPlaylist(selectedPlaylist, singlePlaylistPath);
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Importer comme nouvelle playlist", ImVec2(250, 30)))
            {
                if (PlaylistManager::GetInstance().ImportPlaylist(singlePlaylistPath))
                {
                    playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
                }
            }
        }
    }
}

void UIManager::RenderAudioLibrary() {
    ImGui::Begin("Audio Library Manager");
    if (ImGui::BeginTabBar("AudioLibraryTabs")) {
        if (ImGui::BeginTabItem("Music / Playlists")) {
            RenderMusicPlaylistTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Playlist Manager")) {  
            RenderPlaylistManagerTab();
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
        if (ImGui::BeginTabItem("Wedding Mode")) {
            RenderWeddingModeTab();
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

std::string UIManager::GetDisplayName(const std::string& path) const
{
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos)
    {
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
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Toutes les musiques disponibles");
    
    if (ImGui::BeginTable("AllMusicTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("File Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Sound ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableHeadersRow();
        
        std::vector<std::pair<std::string, std::string>> musicList; 
        const auto& allSounds = AudioManager::GetInstance().GetAllSounds();
        
        for (const auto& kv : allSounds) {
            if (kv.first.find("sfx") == std::string::npos && 
                kv.first.find("announce") == std::string::npos) {
                musicList.push_back({kv.first, kv.second.filePath});
            }
        }
        
        for (size_t i = 0; i < musicList.size(); i++) {
            const auto& [soundId, filePath] = musicList[i];
            
            ImGui::PushID(static_cast<int>(i) + 5000);
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            ImGui::Text("%s", GetDisplayName(filePath).c_str());
            
            ImGui::TableNextColumn();
            ImGui::Text("%s", soundId.c_str());
            
            ImGui::TableNextColumn();
            if (ImGui::Button("Play")) {
                AudioManager::GetInstance().PlaySound(soundId, false, m_musicVolume * m_masterVolume);
            }
            
            ImGui::SameLine();
            
            bool inPlaylist = std::find(playlist->tracks.begin(), playlist->tracks.end(), soundId) != playlist->tracks.end();
            
            if (!inPlaylist) {
                if (ImGui::Button("Add to Playlist")) {
                    PlaylistManager::GetInstance().AddToPlaylist(g_playlistName, soundId);
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.00f), "In Playlist");
            }
            
            ImGui::PopID();
        }
        
        ImGui::EndTable();
    }
}

void UIManager::RenderAnnouncementsTab() {
    if (ImGui::Button("Import Announcement", ImVec2(200, 30))) {
        ImportAnnouncementFiles();
    }
    ImGui::Separator();

    if (ImGui::BeginTable("AnnouncementTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("File Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Sound ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::vector<std::string> announcements;
        const auto& allSounds = AudioManager::GetInstance().GetAllSounds();
        
        for (const auto& [soundId, soundData] : allSounds) {
            if (soundId.find("announce") != std::string::npos || 
                soundId.find("annonce") != std::string::npos ||
                soundId.find("buffet") != std::string::npos) {
                announcements.push_back(soundId);
            }
        }
        
        for (size_t i = 0; i < announcements.size(); i++) {
            const auto& soundId = announcements[i];
            
            ImGui::PushID(static_cast<int>(i) + 2000); 
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            bool selected = (g_selectedAnnouncement == static_cast<int>(i));
            if (ImGui::RadioButton("##sel", selected)) {
                g_selectedAnnouncement = static_cast<int>(i);
                strncpy(g_plannedAnnounceName, soundId.c_str(), sizeof(g_plannedAnnounceName) - 1);
                g_plannedAnnounceName[sizeof(g_plannedAnnounceName) - 1] = '\0';
            }

            ImGui::TableNextColumn();
            auto soundIt = allSounds.find(soundId);
            std::string fileName = "Unknown";
            if (soundIt != allSounds.end()) {
                fileName = GetDisplayName(soundIt->second.filePath);
            }
            ImGui::Text("%s", fileName.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", soundId.c_str());

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    
    ImGui::Separator();
    ImGui::Text("Note: Les contrôles d'annonce sont disponibles dans le panneau 'Announcements'");
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
          
            if (kv.first.find("sfx") != std::string::npos && 
                kv.first != m_weddingEntranceSoundId && 
                kv.first != m_weddingCeremonySoundId && 
                kv.first != m_weddingExitSoundId) {
                sfxList.push_back(kv.first);
            }
        }

        for (size_t i = 0; i < sfxList.size(); i++) {
            const auto& sfxId = sfxList[i];
            ImGui::PushID(static_cast<int>(i) + 3000); 
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            bool selected = (g_selectedSFX == static_cast<int>(i));
            if (ImGui::RadioButton("##sel_sfx", selected)) {
                g_selectedSFX = static_cast<int>(i);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%s", GetDisplayName(sfxId).c_str());

            ImGui::TableNextColumn();
            if (ImGui::Button("Play##sfx_play")) {
                AudioManager::GetInstance().PlaySound(sfxId, false, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete##sfx_delete")) {
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
    if (ImGui::Button("Play")) {
        auto& playlistManager = PlaylistManager::GetInstance();
        
        playlistManager.SetCrossfadeDuration(crossfadeDuration);
        
        playlistManager.Play(playlistName, opts);
        UpdateAllVolumes();
    }

    ImGui::SameLine();

    if (ImGui::Button("Stop")) {
        PlaylistManager::GetInstance().Stop(playlistName);
    }

    ImGui::Separator();
    ImGui::Text("Options de lecture:");
    
    ImGui::Checkbox("Random Order", &opts.randomOrder);
    ImGui::Checkbox("Random Segment", &opts.randomSegment);
    ImGui::Checkbox("Loop Playlist", &opts.loopPlaylist);
    ImGui::SliderFloat("Segment Duration", &opts.segmentDuration, 10.0f, 300.0f, "%.1f s");
    
    if (ImGui::SliderFloat("Crossfade Duration", &crossfadeDuration, 0.0f, 10.0f, "%.1f s")) {
        PlaylistManager::GetInstance().SetCrossfadeDuration(crossfadeDuration);
    }
    
    ImGui::Spacing();
    
    ImGui::Text("Note: Les contrôles de volume sont disponibles dans le panneau 'Volume Controls'");
    ImGui::Text("Note: Les informations sur les annonces sont disponibles dans l'onglet 'Announcements'");
    
    ImGui::Separator();

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
    ImGui::Text("Contrôles des Annonces");
    ImGui::Separator();

    auto& announceManager = AnnouncementManager::GetInstance();
    if (announceManager.IsAnnouncing()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
        ImGui::Text("Annonce en cours: %s", announceManager.GetCurrentAnnouncementName().c_str());
        ImGui::Text("État: %s", announceManager.GetAnnouncementStateString().c_str());
        ImGui::PopStyleColor();
        
        float progress = announceManager.GetAnnouncementProgress();
        
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.7f, 0.0f, 1.0f));
        ImGui::ProgressBar(progress, ImVec2(-1, 0), "Progression de l'annonce");
        ImGui::PopStyleColor();
        
        if (ImGui::Button("Arrêter l'annonce##main_stop", ImVec2(200, 30))) {
            announceManager.StopAnnouncement();
        }
        
        ImGui::Separator();
    }

    ImGui::Text("Annonces disponibles:");
    
    std::vector<std::string> announcements;
    const auto& allSounds = AudioManager::GetInstance().GetAllSounds();
    
    for (const auto& [soundId, soundData] : allSounds) {
        if (soundId.find("announce") != std::string::npos) {
            announcements.push_back(soundId);
        }
    }
    
    static int selectedAnnouncement = -1;
    static char selectedAnnounceName[256] = "";
    
    if (ImGui::BeginListBox("##announcements_list", ImVec2(-FLT_MIN, 150))) {
        for (int i = 0; i < announcements.size(); i++) {
            const bool isSelected = (selectedAnnouncement == i);
            std::string displayName = announcements[i];
            
            auto soundIt = allSounds.find(announcements[i]);
            if (soundIt != allSounds.end()) {
                displayName = GetDisplayName(soundIt->second.filePath);
            }
            
            if (ImGui::Selectable(displayName.c_str(), isSelected)) {
                selectedAnnouncement = i;
                strncpy(selectedAnnounceName, announcements[i].c_str(), sizeof(selectedAnnounceName) - 1);
                selectedAnnounceName[sizeof(selectedAnnounceName) - 1] = '\0';
            }
            
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndListBox();
    }
    
    ImGui::Separator();
    ImGui::Text("Contrôles de lecture:");
    
    static float duckVolume = 0.05f;
    static bool useSFXBefore = true;
    static bool useSFXAfter = true;
    
    ImGui::InputText("Nom de l'annonce", selectedAnnounceName, IM_ARRAYSIZE(selectedAnnounceName));
    ImGui::SliderFloat("Volume de réduction", &duckVolume, 0.0f, 1.0f, "%.2f");
    
    ImGui::Checkbox("SFX avant##ctrl", &useSFXBefore);
    ImGui::SameLine();
    ImGui::Checkbox("SFX après##ctrl", &useSFXAfter);
    
    if (ImGui::Button("Jouer l'annonce##ctrl", ImVec2(200, 30))) {
        AnnouncementManager::GetInstance().PlayAnnouncement(selectedAnnounceName, duckVolume, useSFXBefore, useSFXAfter);
    }
    
    ImGui::SameLine();
    
    if (ImGui::Button("Arrêter l'annonce##ctrl", ImVec2(200, 30))) {
        AnnouncementManager::GetInstance().StopAnnouncement();
    }
    
    ImGui::Separator();
    ImGui::Text("Planification des annonces:");
    
    static int plannedHour = 14;
    static int plannedMinute = 30;
    static char plannedAnnounceName[256] = "";
    
    if (selectedAnnouncement >= 0) {
        strncpy(plannedAnnounceName, selectedAnnounceName, sizeof(plannedAnnounceName) - 1);
        plannedAnnounceName[sizeof(plannedAnnounceName) - 1] = '\0';
    }
    
    ImGui::InputInt("Heure##plan", &plannedHour);
    ImGui::InputInt("Minute##plan", &plannedMinute);
    
    if (plannedHour < 0) plannedHour = 0;
    if (plannedHour > 23) plannedHour = 23;
    if (plannedMinute < 0) plannedMinute = 0;
    if (plannedMinute > 59) plannedMinute = 59;
    
    ImGui::InputText("Annonce à planifier", plannedAnnounceName, IM_ARRAYSIZE(plannedAnnounceName));
    
    if (ImGui::Button("Planifier l'annonce", ImVec2(200, 30))) {
        AnnouncementManager::GetInstance().AddScheduledAnnouncement(
            plannedHour, plannedMinute, plannedAnnounceName);
        spdlog::info("Scheduled announcement '{}' at {:02d}:{:02d}", 
                     plannedAnnounceName, plannedHour, plannedMinute);
    }
    
    ImGui::Separator();
    ImGui::Text("Annonces planifiées:");
    
    static int selectedScheduledAnnouncement = -1;
    static int editHour = 0;
    static int editMinute = 0;
    static char editAnnounceName[256] = "";
    static bool editMode = false;
    
    const auto& scheduledAnnouncements = AnnouncementManager::GetInstance().GetScheduledAnnouncements();
    
    if (ImGui::BeginTable("ScheduledAnnouncementsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Heure", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Minute", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Annonce", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        
        for (size_t i = 0; i < scheduledAnnouncements.size(); i++) {
            const auto& ann = scheduledAnnouncements[i];
            
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            ImGui::Text("%02d", ann.hour);
            
            ImGui::TableNextColumn();
            ImGui::Text("%02d", ann.minute);
            
            ImGui::TableNextColumn();
            ImGui::Text("%s", ann.announcementId.c_str());
            
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i) + 1000);
            
            if (ImGui::Button("Modifier##sched")) {
                selectedScheduledAnnouncement = static_cast<int>(i);
                editMode = true;
                editHour = ann.hour;
                editMinute = ann.minute;
                strncpy(editAnnounceName, ann.announcementId.c_str(), sizeof(editAnnounceName) - 1);
                editAnnounceName[sizeof(editAnnounceName) - 1] = '\0';
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Supprimer##sched")) {
                AnnouncementManager::GetInstance().RemoveScheduledAnnouncement(i);
                if (selectedScheduledAnnouncement == static_cast<int>(i)) {
                    selectedScheduledAnnouncement = -1;
                }
            }
            
            ImGui::PopID();
        }
        
        ImGui::EndTable();
    }
    
    if (editMode && selectedScheduledAnnouncement >= 0) {
        ImGui::Separator();
        ImGui::Text("Modifier l'annonce planifiée");
        
        ImGui::InputInt("Nouvelle heure##edit", &editHour);
        ImGui::InputInt("Nouvelle minute##edit", &editMinute);
        
        if (editHour < 0) editHour = 0;
        if (editHour > 23) editHour = 23;
        if (editMinute < 0) editMinute = 0;
        if (editMinute > 59) editMinute = 59;
        
        ImGui::InputText("Nouvelle annonce##edit", editAnnounceName, sizeof(editAnnounceName));
        
        ImGui::PushID("edit_buttons");
        if (ImGui::Button("Appliquer les modifications", ImVec2(200, 30))) {
            AnnouncementManager::GetInstance().UpdateScheduledAnnouncement(
                selectedScheduledAnnouncement, editHour, editMinute, editAnnounceName);
            editMode = false;
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Annuler", ImVec2(100, 30))) {
            editMode = false;
        }
        ImGui::PopID();
    }
    
    ImGui::Separator();
    
    if (ImGui::Button("Réinitialiser toutes les annonces déclenchées", ImVec2(300, 30))) {
        AnnouncementManager::GetInstance().ResetTriggeredAnnouncements();
    }
    
    ImGui::Separator();
    ImGui::Text("Importer de nouvelles annonces:");
    
    if (ImGui::Button("Importer des fichiers d'annonce", ImVec2(250, 30))) {
        ImportAnnouncementFiles();
    }
}

void UIManager::RenderAudioControls()
{
    ImGui::Text("Volume Controls");
    ImGui::Separator();

    if (ImGui::SliderFloat("Master Volume", &m_masterVolume, 0.0f, 1.0f, "%.2f")) {
        UpdateAllVolumes();
    }

    ImGui::Spacing();

    if (ImGui::SliderFloat("Music Volume", &m_musicVolume, 0.0f, 1.0f, "%.2f")) {
        UpdateAllVolumes();
    }

    if (ImGui::SliderFloat("Announcement Volume", &m_announcementVolume, 0.0f, 3.0f, "%.2f")) {
        UpdateAllVolumes();
    }

    if (ImGui::SliderFloat("SFX Volume", &m_sfxVolume, 0.0f, 3.0f, "%.2f")) {
        UpdateAllVolumes();
    }
    
    ImGui::Spacing();
    
    if (ImGui::SliderFloat("Current Ducking", &m_duckFactor, 0.0f, 1.0f, "%.2f")) {
        UpdateAllVolumes();
    }
    
    ImGui::Text("Current Ducking: Contrôle la réduction du volume pendant les annonces");
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
        bool isSfx = (soundName.find("sfx") != std::string::npos);
        bool isWeddingSound = (soundName == m_weddingEntranceSoundId || 
                              soundName == m_weddingCeremonySoundId || 
                              soundName == m_weddingExitSoundId);

        if (isAnnouncement) {
            baseVolume = m_announcementVolume;
        }
        else if (isSfx && !isWeddingSound) {
            baseVolume = m_sfxVolume;
        }
        else {
            baseVolume = m_musicVolume * m_duckFactor;
        }

        float finalVolume = m_masterVolume * baseVolume;
        
        if (finalVolume < 0.01f) finalVolume = 0.01f;

        for (auto* channel : soundData.channels) {
            if (!channel) continue;
            bool isPlaying = false;
            channel->isPlaying(&isPlaying);

            if (isPlaying) {
                channel->setVolume(finalVolume);
                
                float currentVolume = 0.0f;
                channel->getVolume(&currentVolume);
                
                if (std::abs(currentVolume - finalVolume) > 0.01f) {
                    channel->setVolume(finalVolume);
                }
            }
        }
    }
}

void UIManager::UpdateWeddingMode(float deltaTime)
{
    if (!m_weddingModeActive) return;
    
    if (m_autoDuckingActive) {
        float currentDuckFactor = GetDuckFactor();
        float targetDuckFactor = m_targetDuckFactor;
        
        float newDuckFactor = std::max(targetDuckFactor, currentDuckFactor - (deltaTime / m_crossfadeDuration));
        
        if (newDuckFactor != currentDuckFactor) {
            SetDuckFactor(newDuckFactor);
            UpdateAllVolumes();
            
            spdlog::debug("Ducking progressif: {} -> {}", currentDuckFactor, newDuckFactor);
            
            if (newDuckFactor <= targetDuckFactor + 0.01f) {
                m_autoDuckingActive = false;
                spdlog::info("Ducking progressif terminé pour la phase {}", m_weddingPhase);
            }
        }
    }
    
    CheckWeddingPhaseTransition();
}

void UIManager::CheckWeddingPhaseTransition()
{
    if (!m_weddingModeActive) return;
    
    FMOD::Channel* currentChannel = nullptr;
    
    if (m_weddingPhase == 1) {
        currentChannel = AudioManager::GetInstance().GetLastChannelOfSound(m_weddingEntranceSoundId);
    } else if (m_weddingPhase == 2) {
        currentChannel = AudioManager::GetInstance().GetLastChannelOfSound(m_weddingCeremonySoundId);
    } else if (m_weddingPhase == 3) {
        currentChannel = AudioManager::GetInstance().GetLastChannelOfSound(m_weddingExitSoundId);
    }
    
    if (!currentChannel) return;
    
    FMOD::Sound* currentSound = nullptr;
    unsigned int positionMs = 0;
    unsigned int lengthMs = 0;
    bool isPlaying = false;
    
    currentChannel->isPlaying(&isPlaying);
    if (!isPlaying) {
        if (m_weddingPhase == 3 && m_transitionToNormalMusicAfterWedding) {
            StartNormalMusicAfterWedding();
        }
        return;
    }
    
    if (currentChannel->getCurrentSound(&currentSound) != FMOD_OK || !currentSound) return;
    
    currentSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);
    
    float progress = static_cast<float>(positionMs) / static_cast<float>(lengthMs);
    
    if (m_weddingPhase == 3 && progress >= 0.95f && m_transitionToNormalMusicAfterWedding) {
        StartNormalMusicAfterWedding();
    }
}

bool UIManager::ImportWeddingMusic(int phase, const std::string& filePath)
{
    if (filePath.empty()) return false;
    
    std::string soundId;
    std::string* storedPath = nullptr;
    
    switch (phase) {
        case 1: 
            soundId = m_weddingEntranceSoundId;
            storedPath = &m_weddingEntranceFilePath;
            break;
        case 2:
            soundId = m_weddingCeremonySoundId;
            storedPath = &m_weddingCeremonyFilePath;
            break;
        case 3: 
            soundId = m_weddingExitSoundId;
            storedPath = &m_weddingExitFilePath;
            break;
        default:
            return false;
    }
    
    AudioManager::GetInstance().StopSound(soundId);
    
    if (AudioManager::GetInstance().GetSound(soundId)) {
        AudioManager::GetInstance().UnloadSound(soundId);
    }
    
    bool success = AudioManager::GetInstance().LoadSound(soundId, filePath, true);
    
    if (success) {
        *storedPath = filePath;
        spdlog::info("Musique de mariage phase {} chargée avec succès: {}", phase, filePath);
    } else {
        spdlog::error("Échec du chargement de la musique de mariage phase {}: {}", phase, filePath);
    }
    
    return success;
}

void UIManager::StartNormalMusicAfterWedding()
{
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSoundsWithFadeOut(); 
    
    SetDuckFactor(1.0f);
    UpdateAllVolumes();
    
    m_weddingModeActive = false;
    m_weddingPhase = 0;
    m_autoDuckingActive = false;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = false;
    
    PlaylistOptions opts;
    opts.loopPlaylist = true;
    opts.randomOrder = true;
    opts.randomSegment = true;
    opts.segmentDuration = 30.0f;
    
    PlaylistManager::GetInstance().Play(m_normalPlaylistAfterWedding, opts);
    
    spdlog::info("Transition vers la playlist normale '{}' après la cérémonie de mariage", m_normalPlaylistAfterWedding);
}

void UIManager::RenderWeddingModeTab()
{
    static char normalPlaylistName[256] = "playlist_test";
    static float ceremonyDuckingFactor = 0.3f;
    static bool isWeddingModeInitialized = false;
    static float crossfadeDuration = 5.0f;
    static bool transitionToNormalMusic = true;
    
    ImGui::Text("Mode Mariage");
    ImGui::Separator();
    
    if (ImGui::CollapsingHeader("Configuration du Mode Mariage", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Musiques actuellement chargées pour la cérémonie:");
        
        bool entranceLoaded = AudioManager::GetInstance().GetSound(m_weddingEntranceSoundId) != nullptr;
        bool ceremonyLoaded = AudioManager::GetInstance().GetSound(m_weddingCeremonySoundId) != nullptr;
        bool exitLoaded = AudioManager::GetInstance().GetSound(m_weddingExitSoundId) != nullptr;
        
        ImGui::TextColored(entranceLoaded ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                          "Musique d'entrée: %s", 
                          entranceLoaded ? GetDisplayName(m_weddingEntranceFilePath).c_str() : "Non chargée");
        
        ImGui::TextColored(ceremonyLoaded ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                          "Musique de cérémonie: %s", 
                          ceremonyLoaded ? GetDisplayName(m_weddingCeremonyFilePath).c_str() : "Non chargée");
        
        ImGui::TextColored(exitLoaded ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                          "Musique de sortie: %s", 
                          exitLoaded ? GetDisplayName(m_weddingExitFilePath).c_str() : "Non chargée");
        
        if (ImGui::Button("Mettre à jour les chemins de fichiers", ImVec2(250, 30))) {
            UpdateWeddingFilePaths();
        }
        
        ImGui::Separator();
        ImGui::Text("Importation des musiques pour la cérémonie:");
        
        if (ImGui::Button("Importer musique d'entrée", ImVec2(200, 30))) {
            auto filePaths = OpenFileDialogMultiSelect();
            if (!filePaths.empty()) {
                if (ImportWeddingMusic(1, filePaths[0])) {
                    isWeddingModeInitialized = true;
                }
            }
        }
        
        if (!m_weddingEntranceFilePath.empty()) {
            ImGui::SameLine();
            ImGui::Text("%s", GetDisplayName(m_weddingEntranceFilePath).c_str());
        }
        
        if (ImGui::Button("Importer musique de cérémonie", ImVec2(200, 30))) {
            auto filePaths = OpenFileDialogMultiSelect();
            if (!filePaths.empty()) {
                if (ImportWeddingMusic(2, filePaths[0])) {
                    isWeddingModeInitialized = true;
                }
            }
        }
        
        if (!m_weddingCeremonyFilePath.empty()) {
            ImGui::SameLine();
            ImGui::Text("%s", GetDisplayName(m_weddingCeremonyFilePath).c_str());
        }
        
        if (ImGui::Button("Importer musique de sortie", ImVec2(200, 30))) {
            auto filePaths = OpenFileDialogMultiSelect();
            if (!filePaths.empty()) {
                if (ImportWeddingMusic(3, filePaths[0])) {
                    isWeddingModeInitialized = true;
                }
            }
        }
        
        if (!m_weddingExitFilePath.empty()) {
            ImGui::SameLine();
            ImGui::Text("%s", GetDisplayName(m_weddingExitFilePath).c_str());
        }
        
        ImGui::Separator();
        
        if (ImGui::SliderFloat("Facteur de ducking pendant la cérémonie", &ceremonyDuckingFactor, 0.0f, 1.0f)) {
            m_targetDuckFactor = ceremonyDuckingFactor;
        }
        
        if (ImGui::SliderFloat("Durée du crossfade (secondes)", &crossfadeDuration, 1.0f, 10.0f)) {
            m_crossfadeDuration = crossfadeDuration;
        }
        
        ImGui::Checkbox("Transition automatique entre les phases", &m_autoTransitionToPhase2);
        ImGui::Checkbox("Transition vers musique normale après la cérémonie", &transitionToNormalMusic);
        m_transitionToNormalMusicAfterWedding = transitionToNormalMusic;
        
        if (transitionToNormalMusic) {
            ImGui::InputText("Playlist normale après mariage", normalPlaylistName, IM_ARRAYSIZE(normalPlaylistName));
            
            if (ImGui::Button("Sélectionner playlist", ImVec2(150, 25))) {
                ImGui::OpenPopup("select_normal_playlist");
            }
            
            if (ImGui::BeginPopup("select_normal_playlist")) {
                ImGui::Text("Sélectionner la playlist normale");
                ImGui::Separator();
                
                for (const auto& playlist : PlaylistManager::GetInstance().GetAllPlaylists()) {
                    if (ImGui::Selectable(playlist.name.c_str())) {
                        strncpy(normalPlaylistName, playlist.name.c_str(), sizeof(normalPlaylistName) - 1);
                        normalPlaylistName[sizeof(normalPlaylistName) - 1] = '\0';
                        m_normalPlaylistAfterWedding = normalPlaylistName;
                    }
                }
                ImGui::EndPopup();
            }
        }
        
        isWeddingModeInitialized = !m_weddingEntranceFilePath.empty() && 
                                  !m_weddingCeremonyFilePath.empty() && 
                                  !m_weddingExitFilePath.empty();
        
        if (!isWeddingModeInitialized) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), 
                "Veuillez importer toutes les musiques pour initialiser le Mode Mariage");
        }
    }
    
    ImGui::Separator();
    
    ImGui::Text("Contrôles de la Cérémonie de Mariage");
    
    if (!isWeddingModeInitialized) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), 
            "Veuillez importer toutes les musiques avant d'utiliser les contrôles");
    } else {
        if (ImGui::Button("Phase 1: Entrée de la cérémonie", ImVec2(300, 50))) {
            PlaylistManager::GetInstance().Stop("");
            AudioManager::GetInstance().StopAllSounds();
            
            m_originalDuckFactor = 1.0f;
            SetDuckFactor(m_originalDuckFactor);
            
            FMOD::Channel* channel = AudioManager::GetInstance().PlaySound(m_weddingEntranceSoundId, false, m_musicVolume * m_masterVolume);
            
            m_weddingModeActive = true;
            m_weddingPhase = 1;
            m_autoDuckingActive = true;
            m_autoTransitionToPhase2 = false;
            m_transitionToNormalMusicAfterWedding = transitionToNormalMusic;
            
            m_crossfadeDuration = 3.0f;
            
            UpdateAllVolumes();
            
            spdlog::info("Phase 1 de mariage démarrée avec ducking progressif de 3 secondes");
        }
        
        if (ImGui::Button("Phase 2: Pendant la cérémonie (avec ducking)", ImVec2(300, 50))) {
            PlaylistManager::GetInstance().Stop("");
            AudioManager::GetInstance().StopAllSounds();
            
            m_originalDuckFactor = 1.0f;
            SetDuckFactor(m_originalDuckFactor);
            
            FMOD::Channel* channel = AudioManager::GetInstance().PlaySound(m_weddingCeremonySoundId, true, m_musicVolume * m_masterVolume);
            
            m_weddingModeActive = true;
            m_weddingPhase = 2;
            m_autoDuckingActive = true;
            m_autoTransitionToPhase2 = false;
            m_transitionToNormalMusicAfterWedding = transitionToNormalMusic;
            
            m_crossfadeDuration = 3.0f;
            m_targetDuckFactor = ceremonyDuckingFactor;
            
            UpdateAllVolumes();
            
            spdlog::info("Phase 2 de mariage démarrée avec ducking progressif de 3 secondes");
        }
        
        if (ImGui::Button("Phase 3: Fin de la cérémonie", ImVec2(300, 50))) {
            PlaylistManager::GetInstance().Stop("");
            AudioManager::GetInstance().StopAllSounds();
            
            m_originalDuckFactor = 1.0f;
            SetDuckFactor(m_originalDuckFactor);
            
            FMOD::Channel* channel = AudioManager::GetInstance().PlaySound(m_weddingExitSoundId, false, m_musicVolume * m_masterVolume);
            
            m_weddingModeActive = true;
            m_weddingPhase = 3;
            m_autoDuckingActive = true;
            m_autoTransitionToPhase2 = false;
            m_transitionToNormalMusicAfterWedding = transitionToNormalMusic;
            
            m_crossfadeDuration = 3.0f;
            
            m_normalPlaylistAfterWedding = normalPlaylistName;
            
            UpdateAllVolumes();
            
            spdlog::info("Phase 3 de mariage démarrée avec ducking progressif de 3 secondes");
        }
        
        if (ImGui::Button("Arrêter toutes les musiques", ImVec2(300, 30))) {
            PlaylistManager::GetInstance().Stop("");  
            AudioManager::GetInstance().StopAllSounds(); 
            
            SetDuckFactor(1.0f);
            UpdateAllVolumes();
            
            m_weddingModeActive = false;
            m_weddingPhase = 0;
            m_autoDuckingActive = false;
            m_autoTransitionToPhase2 = false;
            m_transitionToNormalMusicAfterWedding = false;
            
            spdlog::info("Toutes les musiques ont été arrêtées");
        }
        
        if (m_weddingModeActive) {
            ImGui::Separator();
            ImGui::Text("État actuel:");
            
            if (m_weddingPhase == 1) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Phase 1: Entrée de la cérémonie en cours");
                
                if (m_autoDuckingActive) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "Ducking automatique actif");
                }
            } else if (m_weddingPhase == 2) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Phase 2: Cérémonie en cours");
            } else if (m_weddingPhase == 3) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Phase 3: Fin de la cérémonie en cours");
                
                if (m_transitionToNormalMusicAfterWedding) {
                    ImGui::TextColored(ImVec4(0.0f, 0.7f, 1.0f, 1.0f), 
                        "Transition vers la playlist normale '%s' après la fin", m_normalPlaylistAfterWedding.c_str());
                }
            }
            
            FMOD::Channel* currentChannel = nullptr;
            
            if (m_weddingPhase == 1) {
                currentChannel = AudioManager::GetInstance().GetLastChannelOfSound(m_weddingEntranceSoundId);
            } else if (m_weddingPhase == 2) {
                currentChannel = AudioManager::GetInstance().GetLastChannelOfSound(m_weddingCeremonySoundId);
            } else if (m_weddingPhase == 3) {
                currentChannel = AudioManager::GetInstance().GetLastChannelOfSound(m_weddingExitSoundId);
            }
            
            if (currentChannel) {
                FMOD::Sound* currentSound = nullptr;
                unsigned int positionMs = 0;
                unsigned int lengthMs = 0;
                bool isPlaying = false;

                currentChannel->isPlaying(&isPlaying);
                
                if (isPlaying && 
                    currentChannel->getCurrentSound(&currentSound) == FMOD_OK && 
                    currentSound) {
                    currentSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
                    currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);

                    float progress = static_cast<float>(positionMs) / static_cast<float>(lengthMs);
                    
                    float remainingTimeMs = lengthMs - positionMs;
                    float remainingMinutes = floorf((remainingTimeMs / 1000.0f) / 60.0f);
                    float remainingSeconds = fmodf((remainingTimeMs / 1000.0f), 60.0f);
                    
                    char progressText[32];
                    snprintf(progressText, sizeof(progressText), "%.0f:%.02f restant", 
                            remainingMinutes, remainingSeconds);

                    ImGui::ProgressBar(progress, ImVec2(-1, 0), progressText);
                }
            }
        }
    }
}

void UIManager::UpdateWeddingFilePaths()
{
    const auto& allSounds = AudioManager::GetInstance().GetAllSounds();
    
    auto entranceIt = allSounds.find(m_weddingEntranceSoundId);
    if (entranceIt != allSounds.end()) {
        m_weddingEntranceFilePath = entranceIt->second.filePath;
        spdlog::info("Chemin de la musique d'entrée mis à jour: {}", m_weddingEntranceFilePath);
    }
    
    auto ceremonyIt = allSounds.find(m_weddingCeremonySoundId);
    if (ceremonyIt != allSounds.end()) {
        m_weddingCeremonyFilePath = ceremonyIt->second.filePath;
        spdlog::info("Chemin de la musique de cérémonie mis à jour: {}", m_weddingCeremonyFilePath);
    }
    
    auto exitIt = allSounds.find(m_weddingExitSoundId);
    if (exitIt != allSounds.end()) {
        m_weddingExitFilePath = exitIt->second.filePath;
        spdlog::info("Chemin de la musique de sortie mis à jour: {}", m_weddingExitFilePath);
    }
}

void UIManager::PlayRandomMusic() {
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSounds();
    
    PlaylistOptions opts;
    opts.randomOrder = true;
    opts.randomSegment = true;
    opts.loopPlaylist = true;
    opts.segmentDuration = 30.0f;
    
    PlaylistManager::GetInstance().Play("playlist_test", opts);
    UpdateAllVolumes();
    
    spdlog::info("Playlist lancée en mode aléatoire via Bluetooth");
}

void UIManager::StartWeddingPhase1() {
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSounds();
    
    m_originalDuckFactor = 1.0f;
    SetDuckFactor(m_originalDuckFactor);
    
    FMOD::Channel* channel = AudioManager::GetInstance().PlaySound(m_weddingEntranceSoundId, false, m_musicVolume * m_masterVolume);
    
    m_weddingModeActive = true;
    m_weddingPhase = 1;
    m_autoDuckingActive = true;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = false;
    
    m_crossfadeDuration = 3.0f;
    
    UpdateAllVolumes();
    
    spdlog::info("Phase 1 de mariage démarrée via Bluetooth");
}

void UIManager::StartWeddingPhase2() {
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSounds();
    
    m_originalDuckFactor = 1.0f;
    SetDuckFactor(m_originalDuckFactor);
    
    FMOD::Channel* channel = AudioManager::GetInstance().PlaySound(m_weddingCeremonySoundId, true, m_musicVolume * m_masterVolume);
    
    m_weddingModeActive = true;
    m_weddingPhase = 2;
    m_autoDuckingActive = true;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = false;
    
    m_crossfadeDuration = 3.0f;
    m_targetDuckFactor = 0.3f;
    
    UpdateAllVolumes();
    
    spdlog::info("Phase 2 de mariage démarrée via Bluetooth");
}

void UIManager::StartWeddingPhase3() {
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSounds();
    
    m_originalDuckFactor = 1.0f;
    SetDuckFactor(m_originalDuckFactor);
    
    FMOD::Channel* channel = AudioManager::GetInstance().PlaySound(m_weddingExitSoundId, false, m_musicVolume * m_masterVolume);
    
    m_weddingModeActive = true;
    m_weddingPhase = 3;
    m_autoDuckingActive = true;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = false;
    
    m_crossfadeDuration = 3.0f;
    m_targetDuckFactor = 1.0f;  
    
    UpdateAllVolumes();
    
    spdlog::info("Phase 3 de mariage démarrée via Bluetooth");
}

void UIManager::NextWeddingPhase() {
    if (!m_weddingModeActive) {
        StartWeddingPhase1();
        return;
    }
    
    if (m_weddingPhase == 1) {
        StartWeddingPhase2();
    } else if (m_weddingPhase == 2) {
        StartWeddingPhase3();
    } else if (m_weddingPhase == 3) {
        StopAllMusic();
    }
}

void UIManager::StopAllMusic() {
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSounds();
    
    SetDuckFactor(1.0f);
    UpdateAllVolumes();
    
    m_weddingModeActive = false;
    m_weddingPhase = 0;
    m_autoDuckingActive = false;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = false;
    
    spdlog::info("Toutes les musiques ont été arrêtées via Bluetooth");
}


}

