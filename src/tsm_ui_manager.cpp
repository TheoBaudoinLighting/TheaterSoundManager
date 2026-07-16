// tsm_ui_manager.cpp

#include "tsm_ui_manager.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>        
#include <algorithm>    
#include <filesystem>
#include <string>
#include <string_view>
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

namespace
{
std::filesystem::path PathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string FormatDuration(float seconds)
{
    seconds = std::max(seconds, 0.0f);
    const int minutes = static_cast<int>(seconds / 60.0f);
    const float remainingSeconds = std::fmod(seconds, 60.0f);

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d:%05.2f", minutes, remainingSeconds);
    return buffer;
}

template <std::size_t Size>
void CopyToBuffer(char (&destination)[Size], std::string_view source)
{
    const std::size_t count = std::min(source.size(), Size - 1);
    std::memcpy(destination, source.data(), count);
    destination[count] = '\0';
}
}

static char g_newMusicPath[256]       = "";
static char g_newAnnouncementPath[256]= "";
static char g_newSFXPath[256]         = "";

static char g_playlistName[256]       = "playlist_PreShow"; 
static int  g_selectedMusicIndex      = -1; 
static int  g_selectedAnnouncement    = -1; 
static int  g_selectedSFX             = -1;  

static int  g_plannedHour            = 14; 
static int  g_plannedMinute          = 30; 
static char g_plannedAnnounceName[128]= "";

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h> 

std::string WideToUtf8(const wchar_t* text)
{
    if (!text || *text == L'\0') return "";
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

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
            { L"Audio files", L"*.mp3;*.wav;*.ogg" },
            { L"All files", L"*.*" }
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
                            filePaths.push_back(WideToUtf8(filePath));
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

        pFileOpen->SetTitle(L"Select announcement files");

        COMDLG_FILTERSPEC fileTypes[] = {
            { L"Announcement audio files", L"*.mp3;*.wav;*.ogg" },
            { L"All files", L"*.*" }
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
                            filePaths.push_back(WideToUtf8(filePath));
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
        std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        
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

        const auto soundKind = isMusic ? AudioManager::SoundKind::Music
                                       : AudioManager::SoundKind::SoundEffect;
        bool ok = AudioManager::GetInstance().LoadSound(soundID, path, isMusic, soundKind);
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
      m_weddingModeActive(false),
      m_weddingPhase(0),
      m_autoDuckingActive(false),
      m_originalDuckFactor(1.0f),
      m_targetDuckFactor(0.3f),
      m_crossfadeDuration(10.0f),
      m_autoTransitionToPhase2(false),
      m_transitionToNormalMusicAfterWedding(false),
      m_weddingEntranceSoundId("wedding_entrance_sound"),
      m_weddingCeremonySoundId("wedding_ceremony_sound"),
      m_weddingExitSoundId("wedding_exit_sound"),
      m_normalPlaylistAfterWedding("playlist_PostShow")
{
    m_opts.randomOrder = true;      
    m_opts.randomSegment = true;     
    m_opts.loopPlaylist = true;     
    m_opts.segmentDuration = PlaylistOptions::DefaultSegmentDuration;
}

bool UIManager::SelectPlaylist(const std::string& playlistName)
{
    auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(playlistName);
    if (!playlist) return false;

    const bool changed = m_playlistName != playlistName;
    m_playlistName = playlistName;
    m_opts = playlist->options;
    m_crossfadeDuration = playlist->crossfadeDuration;
    if (changed)
    {
        m_playlistFeedback.clear();
        m_playlistFeedbackIsError = false;
    }
    return true;
}

void UIManager::RefreshPlaylistSelection()
{
    auto& playlists = PlaylistManager::GetInstance();
    if (m_playlistName.empty() || playlists.GetPlaylistByName(m_playlistName)) return;

    m_playlistName.clear();
    m_playlistFeedback.clear();
    m_playlistFeedbackIsError = false;
}

bool UIManager::Init(
    int width,
    int height,
    const std::string& resourceRoot,
    const std::string& imguiIniPath)
{
    Shutdown();
    m_isRunning = true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        spdlog::error("SDL initialization failed: {}", SDL_GetError());
        return false;
    }
    m_sdlInitialized = true;

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
        Shutdown();
        return false;
    }
    
    m_glContext = SDL_GL_CreateContext(m_window);
    if (!m_glContext) {
        spdlog::error("OpenGL context creation failed: {}", SDL_GetError());
        Shutdown();
        return false;
    }

    SDL_GL_MakeCurrent(m_window, m_glContext);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_imguiContextCreated = true;
    ImGuiIO& io = ImGui::GetIO();
    m_imguiIniPath = imguiIniPath;
    io.IniFilename = m_imguiIniPath.empty() ? nullptr : m_imguiIniPath.c_str();
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
        Shutdown();
        return false;
    }
    m_imguiSdlInitialized = true;

    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        spdlog::error("ImGui OpenGL initialization failed");
        Shutdown();
        return false;
    }
    m_imguiOpenGlInitialized = true;

    if (!resourceRoot.empty())
    {
        const std::filesystem::path fontPath =
            PathFromUtf8(resourceRoot) / "assets/fonts/Jost-Regular.ttf";
        const std::string fontPathUtf8 = PathToUtf8(fontPath.lexically_normal());
        std::error_code fontError;
        if (std::filesystem::exists(fontPath, fontError) && !fontError)
        {
            if (!io.Fonts->AddFontFromFileTTF(fontPathUtf8.c_str(), 24.0f))
            {
                spdlog::warn(
                    "Unable to load font '{}'; using ImGui's default font.", fontPathUtf8);
                io.Fonts->AddFontDefault();
            }
        }
        else
        {
            spdlog::warn(
                "Font '{}' was not found; using ImGui's default font.", fontPathUtf8);
            io.Fonts->AddFontDefault();
        }
    }
    else
    {
        spdlog::warn("No resource root is configured; using ImGui's default font.");
        io.Fonts->AddFontDefault();
    }

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
    if (m_imguiOpenGlInitialized)
    {
        ImGui_ImplOpenGL3_Shutdown();
        m_imguiOpenGlInitialized = false;
    }
    if (m_imguiSdlInitialized)
    {
        ImGui_ImplSDL2_Shutdown();
        m_imguiSdlInitialized = false;
    }
    if (m_imguiContextCreated)
    {
        ImGui::DestroyContext();
        m_imguiContextCreated = false;
    }

    if (m_glContext) {
        SDL_GL_DeleteContext(m_glContext);
        m_glContext = nullptr;
    }
    
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    if (m_sdlInitialized)
    {
        SDL_Quit();
        m_sdlInitialized = false;
    }
    m_isInitialized = false;
    m_imguiIniPath.clear();
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
    if (!m_isInitialized) return;

    if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) {
        return;
    }

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

    // Add check for minimized window to prevent rendering when minimized
    if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);
        return;
    }

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
    static char renameBuffer[256] = "";
    static bool renameMode = false;
    static bool showImportExportOptions = false;
    static char importExportPath[512] = "playlists.json";
    std::vector<std::string> playlistNames =
        PlaylistManager::GetInstance().GetPlaylistNames();

    if (selectedPlaylistIndex >= static_cast<int>(playlistNames.size()))
    {
        selectedPlaylistIndex = -1;
        renameMode = false;
    }
    ImGui::Text("Available playlists: %zu", playlistNames.size());

    ImGui::Separator();
    ImGui::Text("Create a new playlist");

    ImGui::InputText("Playlist name", newPlaylistName, IM_ARRAYSIZE(newPlaylistName));

    if (ImGui::Button("Create", ImVec2(100, 30)))
    {
        if (strlen(newPlaylistName) > 0)
        {
            PlaylistManager::GetInstance().CreatePlaylist(newPlaylistName);
            playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
            newPlaylistName[0] = '\0';
        }
    }

    ImGui::Separator();
    ImGui::Text("Available playlists");

    if (ImGui::BeginTable("PlaylistsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Tracks", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < playlistNames.size(); i++)
        {
            const auto& playlistName = playlistNames[i];

            ImGui::TableNextRow();
            ImGui::PushID(i);

            // Colonne Name - Selectable SANS SpanAllColumns
            ImGui::TableNextColumn();
            bool selected = (selectedPlaylistIndex == i);
            
            if (ImGui::Selectable(playlistName.c_str(), selected, ImGuiSelectableFlags_None))
            {
                selectedPlaylistIndex = i;
                (void)SelectPlaylist(playlistName);
                if (!renameMode)
                {
                    CopyToBuffer(renameBuffer, playlistName);
                }
            }

            ImGui::TableNextColumn();
            int trackCount = static_cast<int>(PlaylistManager::GetInstance().GetPlaylistTrackCount(playlistName));
            ImGui::Text("%d", trackCount);

            ImGui::TableNextColumn();
            bool isPlaying = PlaylistManager::GetInstance().IsPlaylistPlaying(playlistName);
            ImGui::TextColored(isPlaying ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                            isPlaying ? "Playing" : "Stopped");

            ImGui::TableNextColumn();
            
            if (ImGui::Button("Play", ImVec2(50, 25)))
            {
                // CORRECTION : Utiliser les options de la playlist au lieu des valeurs par défaut
                auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(playlistName);
                if (playlist) {
                    // Utiliser les options configurées de la playlist
                    PlaylistManager::GetInstance().Play(playlistName, playlist->options);
                    
                    // Appliquer aussi la durée de crossfade si elle existe
                    PlaylistManager::GetInstance().SetCrossfadeDuration(playlist->crossfadeDuration);
                    
                    // Mettre à jour le nom de playlist actuel dans l'UIManager pour que les contrôles principaux fonctionnent
                    (void)SelectPlaylist(playlistName);
                } else {
                    // Fallback avec options par défaut si la playlist n'est pas trouvée
                    PlaylistOptions defaultOpts;
                    defaultOpts.randomOrder = true;
                    defaultOpts.loopPlaylist = true;
                    PlaylistManager::GetInstance().Play(playlistName, defaultOpts);
                    
                    // Keep the main controls synchronized with the row action.
                    (void)SelectPlaylist(playlistName);
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Stop", ImVec2(50, 25)))
            {
                PlaylistManager::GetInstance().Stop(playlistName);
            }

            ImGui::SameLine();

            if (ImGui::Button("Rename", ImVec2(60, 25)))
            {
                renameMode = true;
                selectedPlaylistIndex = i;
                CopyToBuffer(renameBuffer, playlistName);
            }

            ImGui::SameLine();

            if (ImGui::Button("Delete", ImVec2(60, 25)))
            {
                ImGui::OpenPopup("Confirm delete");
            }

            if (ImGui::BeginPopupModal("Confirm delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Are you sure you want to delete the playlist '%s'?", playlistName.c_str());
                ImGui::Text("This action is irreversible!");

                if (ImGui::Button("Confirm", ImVec2(120, 30)))
                {
                    PlaylistManager::GetInstance().DeletePlaylist(playlistName);
                    playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
                    if (selectedPlaylistIndex == i)
                    {
                        selectedPlaylistIndex = -1;
                    }
                    RefreshPlaylistSelection();
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();

                if (ImGui::Button("Cancel", ImVec2(120, 30)))
                {
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (renameMode && selectedPlaylistIndex >= 0 && selectedPlaylistIndex < playlistNames.size())
    {
        ImGui::Separator();
        ImGui::Text("Rename playlist '%s'", playlistNames[selectedPlaylistIndex].c_str());

        ImGui::InputText("New name", renameBuffer, IM_ARRAYSIZE(renameBuffer));

        if (ImGui::Button("Apply", ImVec2(100, 30)))
        {
            if (strlen(renameBuffer) > 0)
            {
                PlaylistManager::GetInstance().RenamePlaylist(
                    playlistNames[selectedPlaylistIndex], renameBuffer);
                playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
                const auto renamed = std::find(
                    playlistNames.begin(), playlistNames.end(), renameBuffer);
                selectedPlaylistIndex = renamed == playlistNames.end()
                    ? -1
                    : static_cast<int>(std::distance(playlistNames.begin(), renamed));
                if (selectedPlaylistIndex >= 0)
                    (void)SelectPlaylist(renameBuffer);
                else
                    RefreshPlaylistSelection();
                renameMode = false;
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(100, 30)))
        {
            renameMode = false;
        }
    }

    if (selectedPlaylistIndex >= 0 && selectedPlaylistIndex < playlistNames.size())
    {
        const std::string& selectedPlaylist = playlistNames[selectedPlaylistIndex];
        auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(selectedPlaylist);

        if (playlist)
        {
            ImGui::Separator();
            ImGui::Text("Playlist settings: %s", selectedPlaylist.c_str());

            // AMÉLIORATION : Rendre les contrôles plus intuitifs
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 0.7f, 0.2f, 0.3f));
            
            bool optionsChanged = false;
            
            if (ImGui::Checkbox("Random order", const_cast<bool*>(&playlist->options.randomOrder))) {
                optionsChanged = true;
            }
            ImGui::SameLine();
            
            if (ImGui::Checkbox("Random segment", const_cast<bool*>(&playlist->options.randomSegment))) {
                optionsChanged = true;
            }
            ImGui::SameLine();
            
            if (ImGui::Checkbox("Loop playlist", const_cast<bool*>(&playlist->options.loopPlaylist))) {
                optionsChanged = true;
            }
            
            if (ImGui::SliderFloat("Segment duration", const_cast<float*>(&playlist->options.segmentDuration), 10.0f, 1800.0f, "%.1fs")) {
                optionsChanged = true;
            }
            
            // AJOUT : Contrôle du crossfade
            if (ImGui::SliderFloat("Crossfade duration", const_cast<float*>(&playlist->crossfadeDuration), 0.0f, 10.0f, "%.1fs")) {
                optionsChanged = true;
                // Appliquer immédiatement si la playlist est en cours de lecture
                if (PlaylistManager::GetInstance().IsPlaylistPlaying(selectedPlaylist)) {
                    PlaylistManager::GetInstance().SetCrossfadeDuration(playlist->crossfadeDuration);
                }
            }
            
            ImGui::PopStyleColor();
            
            // Indicateur visuel si des changements ont été faits
            if (optionsChanged) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "(Settings updated)");
            }
            
            // Bouton pour appliquer les changements à une playlist en cours
            if (PlaylistManager::GetInstance().IsPlaylistPlaying(selectedPlaylist)) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "This playlist is currently playing");
                
                if (ImGui::Button("Apply settings to current playback", ImVec2(300, 30))) {
                    // Redémarrer la playlist avec les nouveaux paramètres
                    PlaylistManager::GetInstance().Stop(selectedPlaylist);
                    PlaylistManager::GetInstance().Play(selectedPlaylist, playlist->options);
                    PlaylistManager::GetInstance().SetCrossfadeDuration(playlist->crossfadeDuration);
                    
                    spdlog::info("Applied new settings to playlist '{}'", selectedPlaylist);
                }
                
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Restart to apply changes");
            }

            ImGui::Separator();
            ImGui::Text("Tracks in playlist:");

            if (ImGui::BeginTable("TracksTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
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

                    if (ImGui::Button("Play", ImVec2(50, 25)))
                    {
                        // CORRECTION : Utiliser les options de la playlist pour PlayFromIndex aussi
                        PlaylistManager::GetInstance().Stop(selectedPlaylist);
                        PlaylistManager::GetInstance().PlayFromIndex(selectedPlaylist, static_cast<int>(i));
                        PlaylistManager::GetInstance().SetCrossfadeDuration(playlist->crossfadeDuration);
                        
                        // Mettre à jour le nom de playlist actuel dans l'UIManager
                        (void)SelectPlaylist(selectedPlaylist);
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

                    if (ImGui::Button("Remove", ImVec2(70, 25)))
                    {
                        PlaylistManager::GetInstance().RemoveFromPlaylistAtIndex(selectedPlaylist, i);
                    }

                    ImGui::PopID();
                }

                ImGui::EndTable();
            }

            // Reste du code pour l'ajout de tracks...
            static int selectedTrackToAdd = -1;
            static std::vector<std::pair<std::string, std::string>> availableTracks;

            if (ImGui::Button("Refresh available tracks", ImVec2(250, 30)))
            {
                availableTracks.clear();

                for (const auto& [soundId, soundData] : AudioManager::GetInstance().GetAllSounds())
                {
                    if (soundData.kind == AudioManager::SoundKind::Music)
                    {
                        availableTracks.push_back({soundId, GetDisplayName(soundData.filePath)});
                    }
                }

                std::sort(availableTracks.begin(), availableTracks.end(),
                    [](const auto& a, const auto& b) { return a.second < b.second; });
            }

            if (availableTracks.empty())
            {
                if (ImGui::Button("Load available tracks", ImVec2(250, 30)))
                {
                    availableTracks.clear();

                    for (const auto& [soundId, soundData] : AudioManager::GetInstance().GetAllSounds())
                    {
                        if (soundData.kind == AudioManager::SoundKind::Music)
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
                ImGui::Text("Add a track to the playlist:");

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
                    if (ImGui::Button("Add to playlist", ImVec2(200, 30)))
                    {
                        PlaylistManager::GetInstance().AddToPlaylist(
                            selectedPlaylist, availableTracks[selectedTrackToAdd].first);
                        selectedTrackToAdd = -1;
                    }
                }
            }
        }
    }

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Import/Export options", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputText("File path", importExportPath, IM_ARRAYSIZE(importExportPath));

        if (ImGui::Button("Import all playlists", ImVec2(250, 30)))
        {
            if (PlaylistManager::GetInstance().LoadPlaylistsFromFile(importExportPath))
            {
                playlistNames = PlaylistManager::GetInstance().GetPlaylistNames();
                selectedPlaylistIndex = -1;
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Export all playlists", ImVec2(250, 30)))
        {
            PlaylistManager::GetInstance().SavePlaylistsToFile(importExportPath);
        }

        if (selectedPlaylistIndex >= 0 && selectedPlaylistIndex < playlistNames.size())
        {
            const std::string& selectedPlaylist = playlistNames[selectedPlaylistIndex];

            ImGui::Separator();
            ImGui::Text("Import/Export for selected playlist: %s", selectedPlaylist.c_str());

            static char singlePlaylistPath[512] = "playlist_single.json";
            ImGui::InputText("Single file path", singlePlaylistPath, IM_ARRAYSIZE(singlePlaylistPath));

            if (ImGui::Button("Export this playlist", ImVec2(200, 30)))
            {
                PlaylistManager::GetInstance().ExportPlaylist(selectedPlaylist, singlePlaylistPath);
            }

            ImGui::SameLine();

            if (ImGui::Button("Import as new playlist", ImVec2(250, 30)))
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
    static bool selectNormalModeOnFirstFrame = true;

    ImGui::Begin("Audio Library Manager");
    if (ImGui::BeginTabBar("AudioLibraryTabs")) {
        const ImGuiTabItemFlags normalTabFlags = selectNormalModeOnFirstFrame
            ? ImGuiTabItemFlags_SetSelected
            : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem("Music / Playlists", nullptr, normalTabFlags)) {
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
        selectNormalModeOnFirstFrame = false;
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
                PlaylistManager::GetInstance().PlayFromIndex(g_playlistName, static_cast<int>(i));
                // Mettre à jour le nom de playlist actuel dans l'UIManager
                m_playlistName = g_playlistName;
            }
            ImGui::SameLine();
            
            if (i > 0) {
                if (ImGui::Button("Up")) {
                    PlaylistManager::GetInstance().MoveTrackUp(g_playlistName, static_cast<int>(i));
                    if (g_selectedMusicIndex == static_cast<int>(i)) {
                        g_selectedMusicIndex--;
                    }
                }
                ImGui::SameLine();
            }
            
            if (i < playlist->tracks.size() - 1) {
                if (ImGui::Button("Down")) {
                    PlaylistManager::GetInstance().MoveTrackDown(g_playlistName, static_cast<int>(i));
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
    ImGui::Text("All available music");
    
    if (ImGui::BeginTable("AllMusicTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("File Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Sound ID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableHeadersRow();
        
        std::vector<std::pair<std::string, std::string>> musicList; 
        const auto& allSounds = AudioManager::GetInstance().GetAllSounds();
        
        for (const auto& kv : allSounds) {
            if (kv.second.kind == AudioManager::SoundKind::Music) {
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
                AudioManager::GetInstance().PlayMusic(soundId);
            }
            
            ImGui::SameLine();
            
            bool inPlaylist = std::find(playlist->tracks.begin(), playlist->tracks.end(), soundId) != playlist->tracks.end();
            
            if (!inPlaylist) {
                if (ImGui::Button("Add to Playlist")) {
                    PlaylistManager::GetInstance().AddToPlaylist(g_playlistName, soundId);
                    // Si c'est la playlist active, mettre à jour m_playlistName
                    if (PlaylistManager::GetInstance().IsPlaylistPlaying(g_playlistName)) {
                        m_playlistName = g_playlistName;
                    }
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
                CopyToBuffer(g_plannedAnnounceName, soundId);
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
    ImGui::Text("Note: The announcement controls are available in the 'Announcements' panel");
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
    auto& playlistManager = PlaylistManager::GetInstance();
    RefreshPlaylistSelection();
    const std::vector<std::string> playlistNames = playlistManager.GetPlaylistNames();

    ImGui::TextUnformatted("Programme playback");
    ImGui::SetNextItemWidth(360.0f);
    const char* preview = m_playlistName.empty()
        ? "No playlist"
        : m_playlistName.c_str();
    if (ImGui::BeginCombo("Playlist", preview))
    {
        const bool noPlaylistSelected = m_playlistName.empty();
        if (ImGui::Selectable("No playlist", noPlaylistSelected))
        {
            m_playlistName.clear();
            m_playlistFeedback.clear();
            m_playlistFeedbackIsError = false;
        }
        if (noPlaylistSelected) ImGui::SetItemDefaultFocus();

        if (!playlistNames.empty()) ImGui::Separator();
        for (const std::string& playlistName : playlistNames)
        {
            const bool selected = playlistName == m_playlistName;
            if (ImGui::Selectable(playlistName.c_str(), selected))
                (void)SelectPlaylist(playlistName);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    auto* selectedPlaylist = m_playlistName.empty()
        ? nullptr
        : playlistManager.GetPlaylistByName(m_playlistName);
    const bool hasTracks = selectedPlaylist && !selectedPlaylist->tracks.empty();
    const bool selectedIsPlaying = selectedPlaylist &&
        playlistManager.IsPlaylistPlaying(m_playlistName);
    const bool playbackBlocked = AudioManager::GetInstance().IsNormalPlaybackBlocked();

    if (selectedPlaylist)
    {
        ImGui::Text("%zu track%s", selectedPlaylist->tracks.size(),
                    selectedPlaylist->tracks.size() == 1 ? "" : "s");
        ImGui::SameLine();
        ImGui::TextColored(
            selectedIsPlaying ? ImVec4(0.25f, 0.85f, 0.35f, 1.0f)
                              : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
            selectedIsPlaying ? "Playing" : "Stopped");
    }
    else
    {
        if (playlistNames.empty())
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                "No playlist is configured. Import or create one in the audio library.");
        }
        else
        {
            ImGui::TextColored(
                ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                "Select a playlist to enable programme playback.");
        }
    }

    if (playbackBlocked)
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "Playback inhibited by the cinema safety gate.");
    }
    else if (selectedPlaylist && !hasTracks)
    {
        ImGui::TextColored(
            ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
            "The selected playlist is empty.");
    }

    const bool canPlay = hasTracks && !playbackBlocked;
    ImGui::BeginDisabled(!canPlay);
    if (ImGui::Button("Play", ImVec2(100.0f, 32.0f)))
    {
        playlistManager.Play(m_playlistName, m_opts);
        playlistManager.SetCrossfadeDuration(m_crossfadeDuration);
        UpdateAllVolumes();
        const bool started = playlistManager.IsPlaylistPlaying(m_playlistName);
        m_playlistFeedback = started
            ? "Playback started."
            : "Unable to start: no loaded playable track is available.";
        m_playlistFeedbackIsError = !started;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedIsPlaying);
    if (ImGui::Button("Stop", ImVec2(100.0f, 32.0f)))
    {
        playlistManager.Stop(m_playlistName);
        m_playlistFeedback = "Playback stopped.";
        m_playlistFeedbackIsError = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedIsPlaying || playbackBlocked);
    if (ImGui::Button("Skip", ImVec2(100.0f, 32.0f)))
    {
        playlistManager.SkipToNextTrack(m_playlistName);
        m_playlistFeedback = "Skipped to the next playable track.";
        m_playlistFeedbackIsError = false;
    }
    ImGui::EndDisabled();

    if (!m_playlistFeedback.empty())
    {
        ImGui::TextColored(
            m_playlistFeedbackIsError
                ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
                : ImVec4(0.35f, 0.85f, 0.45f, 1.0f),
            "%s", m_playlistFeedback.c_str());
    }

    ImGui::Separator();
    ImGui::Text("Playback options:");

    ImGui::BeginDisabled(!selectedPlaylist);
    bool optionsChanged = false;
    optionsChanged |= ImGui::Checkbox("Random order", &m_opts.randomOrder);
    optionsChanged |= ImGui::Checkbox("Random segment", &m_opts.randomSegment);
    optionsChanged |= ImGui::Checkbox("Loop playlist", &m_opts.loopPlaylist);
    optionsChanged |= ImGui::SliderFloat(
        "Segment duration", &m_opts.segmentDuration, 10.0f, 1800.0f, "%.1f s");
    const bool crossfadeChanged = ImGui::SliderFloat(
        "Crossfade duration", &m_crossfadeDuration, 0.0f, 10.0f, "%.1f s");
    optionsChanged |= crossfadeChanged;
    ImGui::EndDisabled();

    if (selectedPlaylist && optionsChanged)
    {
        selectedPlaylist->options = m_opts;
        selectedPlaylist->crossfadeDuration = m_crossfadeDuration;
        if (selectedIsPlaying && crossfadeChanged)
            playlistManager.SetCrossfadeDuration(m_crossfadeDuration);
    }

    ImGui::Spacing();
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

                const std::string totalDuration = FormatDuration(lengthMs / 1000.0f);
                ImGui::Text("Total Duration: %s", totalDuration.c_str());

                float progress = lengthMs > 0
                    ? static_cast<float>(positionMs) / static_cast<float>(lengthMs)
                    : 0.0f;
                progress = std::clamp(progress, 0.0f, 1.0f);

                const float remainingTime = positionMs < lengthMs
                    ? (lengthMs - positionMs) / 1000.0f
                    : 0.0f;
                const std::string progressText = FormatDuration(remainingTime) + " remaining";

                if (isInCrossfade)
                {
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, 
                        ImVec4(0.7f, 0.7f, 1.0f, 1.0f - crossfadeProgress));
                }

                ImGui::ProgressBar(progress, ImVec2(-1, 0), progressText.c_str());

                if (isInCrossfade)
                {
                    ImGui::PopStyleColor();
                }
            }
        }

        if (isInCrossfade)
        {
            char crossfadeText[32];
            snprintf(crossfadeText, sizeof(crossfadeText), "Crossfade: %.1f%%", crossfadeProgress * 100.0f);
            
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.2f, 0.2f, 0.8f));
            ImGui::ProgressBar(crossfadeProgress, ImVec2(-1, 0), crossfadeText);
            ImGui::PopStyleColor();
        }

        if (PlaylistManager::GetInstance().GetSegmentDuration() > 0.0f)
        {
            float segmentProgress = PlaylistManager::GetInstance().GetSegmentProgress();
            const std::string segmentText = FormatDuration(
                PlaylistManager::GetInstance().GetSegmentRemainingTime()) + " remaining";

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
            ImGui::ProgressBar(segmentProgress, ImVec2(-1, 0), segmentText.c_str());
            ImGui::PopStyleColor();
        }
        
    }
}

void UIManager::RenderAnnouncementControls()
{
    ImGui::Text("Announcement controls");
    ImGui::Separator();

    auto& announceManager = AnnouncementManager::GetInstance();
    if (announceManager.IsAnnouncing()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
        ImGui::Text("Current announcement: %s", announceManager.GetCurrentAnnouncementName().c_str());
        ImGui::Text("State: %s", announceManager.GetAnnouncementStateString().c_str());
        ImGui::PopStyleColor();
        
        float progress = announceManager.GetAnnouncementProgress();
        
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.7f, 0.0f, 1.0f));
        ImGui::ProgressBar(progress, ImVec2(-1, 0), "Announcement progress");
        ImGui::PopStyleColor();
        
        if (ImGui::Button("Stop announcement##main_stop", ImVec2(200, 30))) {
            announceManager.StopAnnouncement();
        }
        
        ImGui::Separator();
    }

    ImGui::Text("Available announcements:");
    
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
                CopyToBuffer(selectedAnnounceName, announcements[i]);
            }
            
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndListBox();
    }
    
    ImGui::Separator();
    ImGui::Text("Playback controls:");
    
    static float duckVolume = 0.05f;
    static bool useSFXBefore = true;
    static bool useSFXAfter = true;
    
    ImGui::InputText("Announcement name", selectedAnnounceName, IM_ARRAYSIZE(selectedAnnounceName));
    ImGui::SliderFloat("Duck volume", &duckVolume, 0.0f, 1.0f, "%.2f");
    
    ImGui::Checkbox("SFX before##ctrl", &useSFXBefore);
    ImGui::SameLine();
    ImGui::Checkbox("SFX after##ctrl", &useSFXAfter);
    
    if (ImGui::Button("Play announcement##ctrl", ImVec2(200, 30))) {
        AnnouncementManager::GetInstance().PlayAnnouncement(selectedAnnounceName, duckVolume, useSFXBefore, useSFXAfter);
    }
    
    ImGui::SameLine();
    
    if (ImGui::Button("Stop announcement##ctrl", ImVec2(200, 30))) {
        AnnouncementManager::GetInstance().StopAnnouncement();
    }
    
    ImGui::Separator();
    ImGui::Text("Announcement scheduling:");
    
    static int plannedHour = 14;
    static int plannedMinute = 30;
    static char plannedAnnounceName[256] = "";
    
    if (selectedAnnouncement >= 0) {
        CopyToBuffer(plannedAnnounceName, selectedAnnounceName);
    }
    
    ImGui::InputInt("Hour##plan", &plannedHour);
    ImGui::InputInt("Minute##plan", &plannedMinute);
    
    if (plannedHour < 0) plannedHour = 0;
    if (plannedHour > 23) plannedHour = 23;
    if (plannedMinute < 0) plannedMinute = 0;
    if (plannedMinute > 59) plannedMinute = 59;
    
    ImGui::InputText("Announcement to schedule", plannedAnnounceName, IM_ARRAYSIZE(plannedAnnounceName));
    
    if (ImGui::Button("Schedule announcement", ImVec2(200, 30))) {
        AnnouncementManager::GetInstance().AddScheduledAnnouncement(
            plannedHour, plannedMinute, plannedAnnounceName);
        spdlog::info("Scheduled announcement '{}' at {:02d}:{:02d}", 
                     plannedAnnounceName, plannedHour, plannedMinute);
    }
    
    ImGui::Separator();
    ImGui::Text("Scheduled announcements:");
    
    static int selectedScheduledAnnouncement = -1;
    static int editHour = 0;
    static int editMinute = 0;
    static char editAnnounceName[256] = "";
    static bool editMode = false;
    
    const auto& scheduledAnnouncements = AnnouncementManager::GetInstance().GetScheduledAnnouncements();
    
    if (ImGui::BeginTable("ScheduledAnnouncementsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Hour", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Minute", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Announcement", ImGuiTableColumnFlags_WidthStretch);
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
            
            if (ImGui::Button("Modify##sched")) {
                selectedScheduledAnnouncement = static_cast<int>(i);
                editMode = true;
                editHour = ann.hour;
                editMinute = ann.minute;
                CopyToBuffer(editAnnounceName, ann.announcementId);
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Delete##sched")) {
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
        ImGui::Text("Modify scheduled announcement");
        
        ImGui::InputInt("New hour##edit", &editHour);
        ImGui::InputInt("New minute##edit", &editMinute);
        
        if (editHour < 0) editHour = 0;
        if (editHour > 23) editHour = 23;
        if (editMinute < 0) editMinute = 0;
        if (editMinute > 59) editMinute = 59;
        
        ImGui::InputText("New announcement##edit", editAnnounceName, sizeof(editAnnounceName));
        
        ImGui::PushID("edit_buttons");
        if (ImGui::Button("Apply changes", ImVec2(200, 30))) {
            AnnouncementManager::GetInstance().UpdateScheduledAnnouncement(
                selectedScheduledAnnouncement, editHour, editMinute, editAnnounceName);
            editMode = false;
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Cancel", ImVec2(100, 30))) {
            editMode = false;
        }
        ImGui::PopID();
    }
    
    ImGui::Separator();
    
    if (ImGui::Button("Reset all triggered announcements", ImVec2(300, 30))) {
        AnnouncementManager::GetInstance().ResetTriggeredAnnouncements();
    }
    
    ImGui::Separator();
    ImGui::Text("Import new announcements:");
    
    if (ImGui::Button("Import announcement files", ImVec2(250, 30))) {
        ImportAnnouncementFiles();
    }
}

void UIManager::RenderAudioControls()
{
    ImGui::Text("Volume controls");
    ImGui::Separator();

    float masterVolume = GetMasterVolume();
    if (ImGui::SliderFloat("Master volume", &masterVolume, 0.0f, 1.0f, "%.2f")) {
        SetMasterVolume(masterVolume);
        UpdateAllVolumes();
    }

    ImGui::Spacing();

    float musicVolume = GetMusicVolume();
    if (ImGui::SliderFloat("Music volume", &musicVolume, 0.0f, 1.0f, "%.2f")) {
        SetMusicVolume(musicVolume);
        UpdateAllVolumes();
    }

    float announcementVolume = GetAnnouncementVolume();
    if (ImGui::SliderFloat(
            "Announcement volume", &announcementVolume, 0.0f, 3.0f, "%.2f")) {
        SetAnnouncementVolume(announcementVolume);
        UpdateAllVolumes();
    }

    float sfxVolume = GetSFXVolume();
    if (ImGui::SliderFloat("SFX volume", &sfxVolume, 0.0f, 3.0f, "%.2f")) {
        SetSFXVolume(sfxVolume);
        UpdateAllVolumes();
    }
    
    ImGui::Spacing();
    
    auto& mixer = MixerState::GetInstance();
    float duckFactor = mixer.GetDuckFactor();
    if (ImGui::SliderFloat("Current ducking", &duckFactor, 0.0f, 1.0f, "%.2f")) {
        mixer.SetDuckFactor(duckFactor);
        UpdateAllVolumes();
    }
    
    ImGui::Text("Current ducking: Controls the volume reduction during announcements");
}

void UIManager::RenderDebugInfo()
{
    ImGui::Text("Debug information");
    ImGui::Separator();

    float frameRate = ImGui::GetIO().Framerate;
    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / frameRate, frameRate);

    auto& audioManager = AudioManager::GetInstance();
    const auto& allSounds = audioManager.GetAllSounds();
    const auto soundDisplayName = [&](const std::string& soundId)
    {
        const auto soundIt = allSounds.find(soundId);
        return soundIt != allSounds.end() && !soundIt->second.filePath.empty()
            ? GetDisplayName(soundIt->second.filePath)
            : soundId;
    };

    auto& playlistManager = PlaylistManager::GetInstance();
    ImGui::SeparatorText("Audio transition");
    const std::string currentTrackId = playlistManager.GetCurrentTrackName();
    const std::string nextTrackId = playlistManager.GetNextTrackName();
    const std::string currentTrackName = soundDisplayName(currentTrackId);
    const std::string nextTrackName = soundDisplayName(nextTrackId);
    ImGui::Text("Current: %s", currentTrackName.empty() ? "-" : currentTrackName.c_str());
    ImGui::Text("Next: %s", nextTrackName.empty() ? "-" : nextTrackName.c_str());
    if (ImGui::IsItemHovered() && !nextTrackId.empty())
    {
        ImGui::SetTooltip("Sound ID: %s", nextTrackId.c_str());
    }
    const float transitionSeconds = playlistManager.GetSecondsUntilTransition();
    if (transitionSeconds >= 0.0f)
    {
        ImGui::Text("Transition in: %s", FormatDuration(transitionSeconds).c_str());
    }
    ImGui::Text("Last reason: %s", playlistManager.GetLastTransitionReason());
    ImGui::Text("Crossfade: %s (%.1f%%)",
        playlistManager.IsInCrossfade() ? "active" : "idle",
        playlistManager.GetCrossfadeProgress() * 100.0f);
    ImGui::TextUnformatted("Curve: equal-power | limiter: -1 dB");

    float targetLufs = audioManager.GetLoudnessTarget();
    if (ImGui::SliderFloat("Loudness target", &targetLufs, -24.0f, -10.0f, "%.1f LUFS"))
    {
        audioManager.SetLoudnessTarget(targetLufs);
    }

    ImGui::SeparatorText("Music loudness analysis");
    const auto diagnostics = audioManager.GetLoudnessDiagnostics();
    const AudioManager::LoudnessDiagnostic* activeAnalysis = nullptr;
    const AudioManager::LoudnessDiagnostic* nextAnalysis = nullptr;
    int readyCount = 0;
    int failedCount = 0;
    int queuedCount = 0;
    for (const auto& diagnostic : diagnostics)
    {
        switch (diagnostic.status)
        {
            case AudioManager::LoudnessStatus::Analyzing:
                activeAnalysis = &diagnostic;
                break;
            case AudioManager::LoudnessStatus::Queued:
                ++queuedCount;
                if (!nextAnalysis || diagnostic.queuePosition < nextAnalysis->queuePosition)
                    nextAnalysis = &diagnostic;
                break;
            case AudioManager::LoudnessStatus::Ready:
                ++readyCount;
                break;
            case AudioManager::LoudnessStatus::Failed:
                ++failedCount;
                break;
            case AudioManager::LoudnessStatus::NotQueued:
                break;
        }
    }

    const std::string activeAnalysisName = activeAnalysis
        ? GetDisplayName(activeAnalysis->filePath)
        : "-";
    const std::string nextAnalysisName = nextAnalysis
        ? GetDisplayName(nextAnalysis->filePath)
        : "-";
    ImGui::Text("Analyzing now: %s", activeAnalysisName.c_str());
    ImGui::Text("Next analysis: %s", nextAnalysisName.c_str());
    const int processedCount = readyCount + failedCount;
    ImGui::Text("Progress: %d/%d processed | %d queued | %d failed",
        processedCount, static_cast<int>(diagnostics.size()), queuedCount, failedCount);
    const float analysisProgress = diagnostics.empty()
        ? 0.0f
        : static_cast<float>(processedCount) / static_cast<float>(diagnostics.size());
    ImGui::ProgressBar(analysisProgress, ImVec2(-1.0f, 0.0f));

    if (ImGui::BeginTable("LoudnessDiagnostics", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
        ImVec2(0.0f, 220.0f)))
    {
        ImGui::TableSetupColumn("Track");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("LUFS");
        ImGui::TableSetupColumn("True peak");
        ImGui::TableSetupColumn("Gain");
        ImGui::TableHeadersRow();
        for (const auto& diagnostic : diagnostics)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string displayName = GetDisplayName(diagnostic.filePath);
            ImGui::TextUnformatted(displayName.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("ID: %s\nPath: %s",
                    diagnostic.soundName.c_str(), diagnostic.filePath.c_str());
            }
            ImGui::TableNextColumn();
            if (diagnostic.status == AudioManager::LoudnessStatus::Queued &&
                diagnostic.queuePosition > 0)
            {
                ImGui::Text("queued #%d", diagnostic.queuePosition);
            }
            else if (diagnostic.status == AudioManager::LoudnessStatus::Analyzing)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "analyzing now");
            }
            else
            {
                ImGui::TextUnformatted(AudioManager::LoudnessStatusToString(diagnostic.status));
            }
            ImGui::TableNextColumn();
            if (diagnostic.status == AudioManager::LoudnessStatus::Ready)
                ImGui::Text("%.2f", diagnostic.integratedLufs);
            else
                ImGui::TextUnformatted("-");
            ImGui::TableNextColumn();
            if (diagnostic.status == AudioManager::LoudnessStatus::Ready)
                ImGui::Text("%.2f dB", diagnostic.truePeakDb);
            else
                ImGui::TextUnformatted("-");
            ImGui::TableNextColumn();
            if (diagnostic.status == AudioManager::LoudnessStatus::Ready)
                ImGui::Text("%+.2f dB", diagnostic.gainDb);
            else
                ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
}

void UIManager::UpdateAllVolumes()
{
    MixerState::GetInstance().ApplyAllVolumes();
}

void UIManager::StopAudioBeforeWeddingPhase()
{
    // AnnouncementManager owns its channel handles and duck layer. Reset it
    // before AudioManager clears every channel during a wedding transition.
    AnnouncementManager::GetInstance().StopAnnouncement();
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllNonEmergencyImmediately();
}

void UIManager::UpdateWeddingMode(float deltaTime)
{
    if (!m_weddingModeActive) return;

    if (m_weddingPhase == 1) {
        switch (m_phase1State) {
            case WeddingPhase1State::IDLE:
                break;

            case WeddingPhase1State::FADING_OUT_PREVIOUS: {
                m_phase1DuckTimer += deltaTime;
                float t = m_phase1DuckTimer / m_phase1FadeOutDuration;
                if (t > 1.0f) t = 1.0f;

                float newFactor = m_originalDuckFactor * (1.0f - t);
                SetDuckFactor(newFactor);

                spdlog::debug("Wedding Phase 1: Fade out: {:.2f}/{:.2f}s - Factor: {:.2f}",
                              m_phase1DuckTimer, m_phase1FadeOutDuration, newFactor);

                UpdateAllVolumes();

                if (t >= 1.0f) {
                    spdlog::info("Wedding Phase 1: Fade out complete, stopping all music");

                    StopAudioBeforeWeddingPhase();

                    m_phase1DuckTimer = 0.0f;

                    m_phase1SfxChannel = AudioManager::GetInstance().PlaySound("sfx_shine");

                    if (m_phase1SfxChannel) {
                        spdlog::info("Wedding Phase 1: Playing SFX 'sfx_shine'");
                        m_phase1State = WeddingPhase1State::PLAYING_SFX_BEFORE;
                    } else {
                        spdlog::error("Wedding Phase 1: Failed to play SFX 'sfx_shine'");
                        SetDuckFactor(0.0f);
                        m_phase1State = WeddingPhase1State::DUCKING_IN;
                    }
                }
                break;
            }

            case WeddingPhase1State::PLAYING_SFX_BEFORE: {
                if (m_phase1SfxChannel) {
                    bool isPlaying = false;
                    m_phase1SfxChannel->isPlaying(&isPlaying);

                    if (!isPlaying) {
                        spdlog::info("Wedding Phase 1: SFX finished, waiting for {} seconds", m_phase1WaitDuration);
                        m_phase1SfxChannel = nullptr;
                        m_phase1DuckTimer = 0.0f;
                        m_phase1State = WeddingPhase1State::WAITING_AFTER_SFX;
                    }
                }
                else {
                    spdlog::info("Wedding Phase 1: No SFX channel, moving to wait state");
                    m_phase1DuckTimer = 0.0f;
                    m_phase1State = WeddingPhase1State::WAITING_AFTER_SFX;
                }
                break;
            }

            case WeddingPhase1State::WAITING_AFTER_SFX: {
                m_phase1DuckTimer += deltaTime;
                float progress = m_phase1DuckTimer / m_phase1WaitDuration;

                spdlog::debug("Wedding Phase 1: Waiting: {:.2f}/{:.2f}s - Progress: {:.1f}%", 
                             m_phase1DuckTimer, m_phase1WaitDuration, progress * 100.0f);

                if (m_phase1DuckTimer >= m_phase1WaitDuration) {
                    spdlog::info("Wedding Phase 1: Wait complete, starting entrance music with ducking");

                    SetDuckFactor(0.0f);
                    m_phase1EntranceChannel = AudioManager::GetInstance().PlayMusic(
                        m_weddingEntranceSoundId);

                    if (!m_phase1EntranceChannel) {
                        spdlog::error("Wedding Phase 1: entrance music asset is unavailable; cancelling wedding mode");
                        m_weddingModeActive = false;
                        m_weddingPhase = 0;
                        m_phase1State = WeddingPhase1State::IDLE;
                        SetDuckFactor(1.0f);
                        UpdateAllVolumes();
                        break;
                    }

                    UpdateAllVolumes();

                    m_phase1DuckTimer = 0.0f;
                    m_phase1State = WeddingPhase1State::DUCKING_IN;
                }
                break;
            }

            case WeddingPhase1State::DUCKING_IN: {
                m_phase1DuckTimer += deltaTime;
                float t = m_phase1DuckTimer / m_phase1DuckFadeDuration;
                if (t > 1.0f) t = 1.0f;

                float newFactor = t;
                SetDuckFactor(newFactor);

                spdlog::debug("Wedding Phase 1: Duck fade: {:.2f}/{:.2f}s - Factor: {:.2f} (20 second fade)", 
                              m_phase1DuckTimer, m_phase1DuckFadeDuration, newFactor);

                UpdateAllVolumes();

                if (t >= 1.0f) {
                    spdlog::info("Wedding Phase 1: 20-second ducking complete, music at full volume");
                    m_phase1State = WeddingPhase1State::PLAYING_ENTRANCE;
                }
                break;
            }

            case WeddingPhase1State::PLAYING_ENTRANCE: {
                if (m_phase1EntranceChannel) {
                    bool isPlaying = false;
                    m_phase1EntranceChannel->isPlaying(&isPlaying);
                    if (!isPlaying) {
                        m_phase1EntranceChannel = nullptr;
                        if (m_autoTransitionToPhase2) {
                            StartWeddingPhase2(m_transitionToNormalMusicAfterWedding);
                        } else {
                            m_phase1DuckTimer = 0.0f;
                            m_phase1State = WeddingPhase1State::DUCKING_OUT;
                        }
                    }
                }
                break;
            }

            case WeddingPhase1State::DUCKING_OUT: {
                m_phase1DuckTimer += deltaTime;
                float t = m_phase1DuckTimer / m_phase1DuckFadeDuration;
                if (t > 1.0f) t = 1.0f;

                float newFactor = m_targetDuckFactor + (1.0f - m_targetDuckFactor) * t;
                SetDuckFactor(newFactor);
                UpdateAllVolumes();

                if (t >= 1.0f) {
                    m_phase1State = WeddingPhase1State::IDLE;
                }
                break;
            }
        }
        return;
    }

    if (m_autoDuckingActive) {
        m_autoDuckTimer += std::max(deltaTime, 0.0f);
        const float progress = m_crossfadeDuration <= 0.0f
            ? 1.0f
            : std::clamp(m_autoDuckTimer / m_crossfadeDuration, 0.0f, 1.0f);
        const float newDuckFactor = m_autoDuckStartFactor +
            (m_targetDuckFactor - m_autoDuckStartFactor) * progress;
        SetDuckFactor(newDuckFactor);
        UpdateAllVolumes();

        if (progress >= 1.0f) {
            m_autoDuckingActive = false;
            spdlog::info("Progressive ducking finished for phase {}", m_weddingPhase);
        }
    }

    CheckWeddingPhaseTransition();
}

void UIManager::CheckWeddingPhaseTransition()
{
    if (!m_weddingModeActive) return;

    FMOD::Channel* currentChannel = nullptr;

    if (m_weddingPhase == 1) {
        currentChannel = m_phase1EntranceChannel;
    } else if (m_weddingPhase == 2) {
        currentChannel = m_weddingCeremonyChannel;
    } else if (m_weddingPhase == 3) {
        currentChannel = m_weddingExitChannel;
    }

    if (!currentChannel) return;

    FMOD::Sound* currentSound = nullptr;
    unsigned int positionMs = 0;
    unsigned int lengthMs = 0;
    bool isPlaying = false;

    currentChannel->isPlaying(&isPlaying);
    if (!isPlaying) {
        if (m_weddingPhase == 1 && m_phase1State == WeddingPhase1State::PLAYING_ENTRANCE && m_autoTransitionToPhase2) {
            m_phase1EntranceChannel = nullptr;
            spdlog::info("End of phase 1, automatic transition to phase 2");
            StartWeddingPhase2(m_transitionToNormalMusicAfterWedding);
        }
        else if (m_weddingPhase == 2 && m_autoTransitionToPhase2) {
            m_weddingCeremonyChannel = nullptr;
            spdlog::info("End of phase 2, automatic transition to phase 3");
            StartWeddingPhase3(m_transitionToNormalMusicAfterWedding, m_normalPlaylistAfterWedding);
        }
        else if (m_weddingPhase == 3 && m_transitionToNormalMusicAfterWedding) {
            m_weddingExitChannel = nullptr;
            StartNormalMusicAfterWedding();
        }
        return;
    }

    if (currentChannel->getCurrentSound(&currentSound) != FMOD_OK || !currentSound) return;

    currentSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);

    if (lengthMs == 0) return;

    unsigned int remainingMs = lengthMs - positionMs;

    unsigned int transitionStartMs = static_cast<unsigned int>(m_phasesTransitionDuration * 1000);

    if (remainingMs <= transitionStartMs) {
        if (m_weddingPhase == 1 && m_phase1State == WeddingPhase1State::PLAYING_ENTRANCE && m_autoTransitionToPhase2) {
            float t = 1.0f - (static_cast<float>(remainingMs) / transitionStartMs);
            float newDuckFactor = 1.0f * (1.0f - t);

            SetDuckFactor(newDuckFactor);
            UpdateAllVolumes();

            if (remainingMs < 200) {
                spdlog::info("End of phase 1 with transition, moving to phase 2");
                StartWeddingPhase2(m_transitionToNormalMusicAfterWedding);
            }
        }
        else if (m_weddingPhase == 2 && m_autoTransitionToPhase2) {
            float t = 1.0f - (static_cast<float>(remainingMs) / transitionStartMs);
            float newDuckFactor = 1.0f * (1.0f - t);

            SetDuckFactor(newDuckFactor);
            UpdateAllVolumes();

            if (remainingMs < 200) {
                spdlog::info("End of phase 2 with transition, moving to phase 3");
                StartWeddingPhase3(m_transitionToNormalMusicAfterWedding, m_normalPlaylistAfterWedding);
            }
        }
        else if (m_weddingPhase == 3 && m_transitionToNormalMusicAfterWedding) {
            float t = 1.0f - (static_cast<float>(remainingMs) / transitionStartMs);
            float newDuckFactor = 1.0f * (1.0f - t);

            SetDuckFactor(newDuckFactor);
            UpdateAllVolumes();

            if (remainingMs < 200) {
                spdlog::info("End of phase 3 with transition, moving to normal playlist");
                StartNormalMusicAfterWedding();
            }
        }
    }
}

bool UIManager::ImportWeddingMusic(int phase, const std::string& filePath)
{
    if (filePath.empty()) return false;
    if (m_weddingModeActive)
    {
        spdlog::error("Stop the wedding sequence before replacing an active phase asset.");
        return false;
    }

    std::string* storedPath = nullptr;

    switch (phase) {
        case 1: 
            storedPath = &m_weddingEntranceFilePath;
            break;
        case 2:
            storedPath = &m_weddingCeremonyFilePath;
            break;
        case 3: 
            storedPath = &m_weddingExitFilePath;
            break;
        default:
            return false;
    }

    const bool success = AudioManager::GetInstance().LoadWeddingPhaseSound(
        phase, filePath);

    if (success) {
        *storedPath = filePath;
        spdlog::info("Wedding music phase {} loaded successfully: {}", phase, filePath);
    } else {
        spdlog::error("Failed to load wedding music phase {}: {}", phase, filePath);
    }

    return success;
}

void UIManager::StartNormalMusicAfterWedding()
{
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSoundsWithFadeOut(); 

    m_originalDuckFactor = GetDuckFactor();
    SetDuckFactor(1.0f);
    UpdateAllVolumes();

    m_weddingModeActive = false;
    m_weddingPhase = 0;
    m_autoDuckingActive = false;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = false;
    m_phase1SfxChannel = nullptr;
    m_phase1EntranceChannel = nullptr;
    m_weddingCeremonyChannel = nullptr;
    m_weddingExitChannel = nullptr;

    m_playlistName = m_normalPlaylistAfterWedding;

    // Définir les options standard pour la playlist
    PlaylistOptions opts;
    opts.loopPlaylist = true;
    opts.randomOrder = true;
    opts.randomSegment = true;
    opts.segmentDuration = PlaylistOptions::DefaultSegmentDuration;

    // Check if "secret_love" track exists in the playlist
    auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName(m_normalPlaylistAfterWedding);
    bool secretLoveFound = false;
    
    if (playlist) {
        // Appliquer les options à la playlist indépendamment de si on trouve secret_love
        playlist->options = opts;
        
        // Find the "secret_love" track directly by ID
        for (size_t i = 0; i < playlist->tracks.size(); i++) {
            if (playlist->tracks[i] == "secret_love") {
                spdlog::info("Found 'secret_love' track at index {} in playlist, starting from there", i);
                
                // D'abord, activer la playlist avec les options
                PlaylistManager::GetInstance().Play(m_normalPlaylistAfterWedding, opts);
                
                // Ensuite, passer directement à l'index de secret_love
                PlaylistManager::GetInstance().Stop(m_normalPlaylistAfterWedding);
                PlaylistManager::GetInstance().PlayFromIndex(m_normalPlaylistAfterWedding, static_cast<int>(i));
                PlaylistManager::GetInstance().SetCrossfadeDuration(10.0f);
                
                secretLoveFound = true;
                break;
            }
        }
    }
    
    // If the track wasn't found or the playlist doesn't exist, use default settings
    if (!secretLoveFound) {
        spdlog::warn("'secret_love' track not found in playlist, starting with regular settings");
        PlaylistManager::GetInstance().Play(m_normalPlaylistAfterWedding, opts);
        PlaylistManager::GetInstance().SetCrossfadeDuration(10.0f);
    }

    spdlog::info("Transition to normal playlist '{}' after wedding ceremony with fade-in", m_normalPlaylistAfterWedding);
}

void UIManager::RenderWeddingModeTab()
{
    static char normalPlaylistName[256] = "playlist_PostShow";
    static float ceremonyDuckingFactor = 0.3f;
    static bool isWeddingModeInitialized = false;
    static float crossfadeDuration = 5.0f;
    static bool transitionToNormalMusic = true;

    ImGui::Text("Wedding Mode");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Wedding Mode Configuration", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Currently loaded music for the ceremony:");

        bool entranceLoaded = AudioManager::GetInstance().GetSound(m_weddingEntranceSoundId) != nullptr;
        bool ceremonyLoaded = AudioManager::GetInstance().GetSound(m_weddingCeremonySoundId) != nullptr;
        bool exitLoaded = AudioManager::GetInstance().GetSound(m_weddingExitSoundId) != nullptr;

        ImGui::TextColored(entranceLoaded ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                          "Entrance music: %s", 
                          entranceLoaded ? GetDisplayName(m_weddingEntranceFilePath).c_str() : "Not loaded");

        ImGui::TextColored(ceremonyLoaded ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                          "Ceremony music: %s", 
                          ceremonyLoaded ? GetDisplayName(m_weddingCeremonyFilePath).c_str() : "Not loaded");

        ImGui::TextColored(exitLoaded ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f), 
                          "Exit music: %s", 
                          exitLoaded ? GetDisplayName(m_weddingExitFilePath).c_str() : "Not loaded");

        if (ImGui::Button("Update file paths", ImVec2(250, 30))) {
            UpdateWeddingFilePaths();
        }

        ImGui::Separator();
        ImGui::Text("Import music for the ceremony:");

        if (ImGui::Button("Import entrance music", ImVec2(200, 30))) {
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

        if (ImGui::Button("Import ceremony music", ImVec2(200, 30))) {
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

        if (ImGui::Button("Import exit music", ImVec2(200, 30))) {
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

        if (ImGui::SliderFloat("Ducking factor during ceremony", &ceremonyDuckingFactor, 0.0f, 1.0f)) {
            m_targetDuckFactor = ceremonyDuckingFactor;
        }

        if (ImGui::SliderFloat("Crossfade duration (seconds)", &crossfadeDuration, 1.0f, 10.0f)) {
            m_crossfadeDuration = crossfadeDuration;
        }

        ImGui::Checkbox("Automatic transition between phases", &m_autoTransitionToPhase2);
        ImGui::Checkbox("Transition to normal music after ceremony", &transitionToNormalMusic);
        m_transitionToNormalMusicAfterWedding = transitionToNormalMusic;

        if (transitionToNormalMusic) {
            ImGui::InputText("Normal playlist after wedding", normalPlaylistName, IM_ARRAYSIZE(normalPlaylistName));

            if (ImGui::Button("Select playlist", ImVec2(150, 25))) {
                ImGui::OpenPopup("select_normal_playlist");
            }

            if (ImGui::BeginPopup("select_normal_playlist")) {
                ImGui::Text("Select normal playlist");
                ImGui::Separator();

                for (const auto& playlist : PlaylistManager::GetInstance().GetAllPlaylists()) {
                    if (ImGui::Selectable(playlist.name.c_str())) {
                        CopyToBuffer(normalPlaylistName, playlist.name);
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
                "Please import all music to initialize Wedding Mode");
        }
    }

    ImGui::Separator();

    ImGui::Text("Wedding Ceremony Controls");

    if (!isWeddingModeInitialized) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), 
            "Please import all music before using the controls");
    } else {
        if (ImGui::Button("Phase 1: Ceremony Entrance", ImVec2(300, 50))) {
            StartWeddingPhase1(transitionToNormalMusic);
        }

        if (ImGui::Button("Phase 2: During Ceremony (with ducking)", ImVec2(300, 50))) {
            StartWeddingPhase2(transitionToNormalMusic);
        }

        if (ImGui::Button("Phase 3: End of Ceremony", ImVec2(300, 50))) {
            StartWeddingPhase3(transitionToNormalMusic, normalPlaylistName);
        }

        if (ImGui::Button("Stop all music", ImVec2(300, 30))) {
            StopAllMusic();
        }

        if (m_weddingModeActive) {
            ImGui::Separator();

            if (ImGui::Button("Skip to end of phase", ImVec2(300, 40))) {
                if (m_weddingPhase == 1) {
                    if (m_phase1State == WeddingPhase1State::FADING_OUT_PREVIOUS) {
                        spdlog::info("Skip: End of fade out of previous music");
                        StopAudioBeforeWeddingPhase();
                        m_phase1DuckTimer = 0.0f;
                        m_phase1State = WeddingPhase1State::PLAYING_SFX_BEFORE;

                        m_phase1SfxChannel = AudioManager::GetInstance().PlaySound("sfx_shine");
                    }
                    else if (m_phase1State == WeddingPhase1State::PLAYING_SFX_BEFORE) {
                        spdlog::info("Skip: End of SFX");
                        if (m_phase1SfxChannel) {
                            m_phase1SfxChannel->stop();
                            m_phase1SfxChannel = nullptr;
                        }
                        m_phase1DuckTimer = 0.0f;
                        m_phase1State = WeddingPhase1State::WAITING_AFTER_SFX;
                    }
                    else if (m_phase1State == WeddingPhase1State::WAITING_AFTER_SFX) {
                        spdlog::info("Skip: End of waiting after SFX");
                        m_phase1DuckTimer = m_phase1WaitDuration;
                        SetDuckFactor(0.0f);

                        m_phase1EntranceChannel = AudioManager::GetInstance().PlayMusic(
                            m_weddingEntranceSoundId);
                        UpdateAllVolumes();

                        m_phase1DuckTimer = 0.0f;
                        m_phase1State = WeddingPhase1State::DUCKING_IN;
                    }
                    else if (m_phase1State == WeddingPhase1State::DUCKING_IN) {
                        spdlog::info("Skip: End of ducking in");
                        SetDuckFactor(1.0f);
                        UpdateAllVolumes();
                        m_phase1State = WeddingPhase1State::PLAYING_ENTRANCE;
                    }
                    else if (m_phase1State == WeddingPhase1State::PLAYING_ENTRANCE) {
                        spdlog::info("Skip: End of entrance music");
                        if (m_phase1EntranceChannel) {
                            m_phase1EntranceChannel->stop();
                            m_phase1EntranceChannel = nullptr;
                        }
                        StartWeddingPhase2(m_transitionToNormalMusicAfterWedding);
                    }
                }
                else if (m_weddingPhase == 2) {
                    spdlog::info("Skip: End of phase 2");
                    StartWeddingPhase3(m_transitionToNormalMusicAfterWedding, m_normalPlaylistAfterWedding);
                }
                else if (m_weddingPhase == 3) {
                    spdlog::info("Skip: End of phase 3");
                    if (m_transitionToNormalMusicAfterWedding) {
                        StartNormalMusicAfterWedding();
                    }
                    else {
                        StopAllMusic();
                    }
                }
            }

            if (ImGui::Button("Go to next phase", ImVec2(300, 40))) {
                if (m_weddingPhase == 1) {
                    spdlog::info("Direct transition: Phase 1 -> Phase 2");
                    StartWeddingPhase2(m_transitionToNormalMusicAfterWedding);
                }
                else if (m_weddingPhase == 2) {
                    spdlog::info("Direct transition: Phase 2 -> Phase 3");
                    StartWeddingPhase3(m_transitionToNormalMusicAfterWedding, m_normalPlaylistAfterWedding);
                }
                else if (m_weddingPhase == 3) {
                    spdlog::info("Direct transition: Phase 3 -> End");
                    if (m_transitionToNormalMusicAfterWedding) {
                        StartNormalMusicAfterWedding();
                    }
                    else {
                        StopAllMusic();
                    }
                }
            }

            if (ImGui::Button("Test transition to normal playlist", ImVec2(300, 40))) {
                StartNormalMusicAfterWedding();
                spdlog::info("Test transition to normal playlist: {}", m_normalPlaylistAfterWedding);
            }

            if (ImGui::Button("Jump to 30 sec before end", ImVec2(300, 40))) {
                FMOD::Channel* currentChannel = nullptr;

                if (m_weddingPhase == 1 && m_phase1State == WeddingPhase1State::PLAYING_ENTRANCE) {
                    currentChannel = m_phase1EntranceChannel;
                } else if (m_weddingPhase == 2) {
                    currentChannel = m_weddingCeremonyChannel;
                } else if (m_weddingPhase == 3) {
                    currentChannel = m_weddingExitChannel;
                }

                if (currentChannel) {
                    FMOD::Sound* currentSound = nullptr;
                    bool isPlaying = false;
                    currentChannel->isPlaying(&isPlaying);

                    if (isPlaying && currentChannel->getCurrentSound(&currentSound) == FMOD_OK && currentSound) {
                        unsigned int lengthMs = 0;
                        currentSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);

                        unsigned int newPositionMs = 0;
                        if (lengthMs > 30000) {
                            newPositionMs = lengthMs - 30000;
                        }

                        FMOD_RESULT result = currentChannel->setPosition(newPositionMs, FMOD_TIMEUNIT_MS);
                        if (result == FMOD_OK) {
                            spdlog::info("Jumped to 30 seconds before end (position: {}ms / {}ms)", 
                                         newPositionMs, lengthMs);
                        } else {
                            spdlog::error("Unable to set music position");
                        }
                    } else {
                        spdlog::error("Unable to get current sound");
                    }
                } else {
                    spdlog::error("No active channel found for phase {}", m_weddingPhase);
                }
            }

            ImGui::Separator();
            ImGui::Text("Current state:");

            if (m_weddingPhase == 1) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Phase 1: Ceremony entrance in progress");

                const char* stateStr = "Unknown";
                switch (m_phase1State) {
                    case WeddingPhase1State::IDLE: stateStr = "Idle"; break;
                    case WeddingPhase1State::FADING_OUT_PREVIOUS: stateStr = "Fading out previous music"; break;
                    case WeddingPhase1State::PLAYING_SFX_BEFORE: stateStr = "Playing SFX"; break;
                    case WeddingPhase1State::WAITING_AFTER_SFX: stateStr = "Waiting after SFX"; break;
                    case WeddingPhase1State::DUCKING_IN: stateStr = "Ducking in (increasing volume)"; break;
                    case WeddingPhase1State::PLAYING_ENTRANCE: stateStr = "Playing entrance music"; break;
                    case WeddingPhase1State::DUCKING_OUT: stateStr = "Ducking out"; break;
                }
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Sub-phase: %s", stateStr);

                if (m_autoDuckingActive) {
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.0f, 1.0f), "Automatic ducking active");
                }
            } else if (m_weddingPhase == 2) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Phase 2: Ceremony in progress");
            } else if (m_weddingPhase == 3) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Phase 3: End of ceremony in progress");

                if (m_transitionToNormalMusicAfterWedding) {
                    ImGui::TextColored(ImVec4(0.0f, 0.7f, 1.0f, 1.0f), 
                        "Transition to normal playlist '%s' after end", m_normalPlaylistAfterWedding.c_str());
                }
            }

            FMOD::Channel* currentChannel = nullptr;

            if (m_weddingPhase == 1) {
                currentChannel = m_phase1EntranceChannel;
            } else if (m_weddingPhase == 2) 
            {
                currentChannel = m_weddingCeremonyChannel;
            }
            else if (m_weddingPhase == 3)
            {
                currentChannel = m_weddingExitChannel;
            }

            if (currentChannel) {
                FMOD::Sound* currentSound = nullptr;
                unsigned int positionMs = 0;
                unsigned int lengthMs = 0;
                bool isPlaying = false;

                currentChannel->isPlaying(&isPlaying);

                if (isPlaying && currentChannel->getCurrentSound(&currentSound) == FMOD_OK && currentSound) {
                    currentSound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
                    currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);

                    float totalMinutes = floorf((lengthMs / 1000.0f) / 60.0f);
                    float totalSeconds = fmodf((lengthMs / 1000.0f), 60.0f);

                    float currentMinutes = floorf((positionMs / 1000.0f) / 60.0f);
                    float currentSeconds = fmodf((positionMs / 1000.0f), 60.0f);

                    ImGui::Text("Position: %.0f:%.02f / %.0f:%.02f", 
                               currentMinutes, currentSeconds, totalMinutes, totalSeconds);

                    float progress = static_cast<float>(positionMs) / static_cast<float>(lengthMs);
                    ImGui::ProgressBar(progress, ImVec2(-1, 0), "Progress");
                }
            }
        }
    }

    CheckWeddingPhaseTransition();
}

void UIManager::UpdateWeddingFilePaths()
{
    const auto& allSounds = AudioManager::GetInstance().GetAllSounds();
    m_weddingEntranceFilePath.clear();
    m_weddingCeremonyFilePath.clear();
    m_weddingExitFilePath.clear();

    auto entranceIt = allSounds.find(m_weddingEntranceSoundId);
    if (entranceIt != allSounds.end()) {
        m_weddingEntranceFilePath = entranceIt->second.filePath;
        spdlog::info("Entrance music path updated: {}", m_weddingEntranceFilePath);
    }

    auto ceremonyIt = allSounds.find(m_weddingCeremonySoundId);
    if (ceremonyIt != allSounds.end()) {
        m_weddingCeremonyFilePath = ceremonyIt->second.filePath;
        spdlog::info("Ceremony music path updated: {}", m_weddingCeremonyFilePath);
    }

    auto exitIt = allSounds.find(m_weddingExitSoundId);
    if (exitIt != allSounds.end()) {
        m_weddingExitFilePath = exitIt->second.filePath;
        spdlog::info("Exit music path updated: {}", m_weddingExitFilePath);
    }
}

void UIManager::PlayRandomMusic() {
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSoundsWithFadeOut();

    PlaylistOptions opts;
    opts.randomOrder = true;
    opts.randomSegment = true;
    opts.loopPlaylist = true;
    opts.segmentDuration = PlaylistOptions::DefaultSegmentDuration;

    PlaylistManager::GetInstance().Play("playlist_PreShow", opts);
    UpdateAllVolumes();

    spdlog::info("Playlist started in random mode via Bluetooth");
}

void UIManager::StartWeddingPhase1(bool transitionToNormalMusicAfter) {

    m_originalDuckFactor = GetDuckFactor();

    m_weddingCeremonyChannel = nullptr;
    m_weddingExitChannel = nullptr;

    m_weddingModeActive = true;
    m_weddingPhase = 1;
    m_phase1State = WeddingPhase1State::FADING_OUT_PREVIOUS;
    m_phase1DuckTimer = 0.0f;
    m_transitionToNormalMusicAfterWedding = transitionToNormalMusicAfter;

    spdlog::info("Wedding Phase 1: Starting with progressive fade out ({:.1f} seconds)", m_phase1FadeOutDuration);
}

void UIManager::StartWeddingPhase2(bool transitionToNormalMusicAfter) {
    StopAudioBeforeWeddingPhase();

    m_originalDuckFactor = 1.0f;
    SetDuckFactor(0.0f);
    m_autoDuckStartFactor = 0.0f;
    m_autoDuckTimer = 0.0f;

    m_phase1SfxChannel = nullptr;
    m_phase1EntranceChannel = nullptr;
    m_weddingExitChannel = nullptr;

    const bool loopCeremony = !transitionToNormalMusicAfter;
    m_weddingCeremonyChannel = AudioManager::GetInstance().PlayMusic(
        m_weddingCeremonySoundId, loopCeremony);

    if (!m_weddingCeremonyChannel) {
        spdlog::error("Wedding phase 2 music asset is unavailable; cancelling wedding mode");
        m_weddingModeActive = false;
        m_weddingPhase = 0;
        m_autoDuckingActive = false;
        m_autoTransitionToPhase2 = false;
        SetDuckFactor(1.0f);
        UpdateAllVolumes();
        return;
    }

    m_weddingModeActive = true;
    m_weddingPhase = 2;
    m_autoDuckingActive = true;
    m_autoTransitionToPhase2 = transitionToNormalMusicAfter;
    m_transitionToNormalMusicAfterWedding = transitionToNormalMusicAfter;

    m_crossfadeDuration = 20.0f;
    m_targetDuckFactor = 0.05f;

    UpdateAllVolumes();

    spdlog::info("Wedding phase 2 started with 20-second ducking in");
}

void UIManager::StartWeddingPhase3(bool transitionToNormalMusicAfter, const std::string& postWeddingPlaylist) {
    StopAudioBeforeWeddingPhase();

    m_originalDuckFactor = 1.0f;
    m_targetDuckFactor = 1.0f;
    SetDuckFactor(0.0f);
    m_autoDuckStartFactor = 0.0f;
    m_autoDuckTimer = 0.0f;

    m_phase1SfxChannel = nullptr;
    m_phase1EntranceChannel = nullptr;
    m_weddingCeremonyChannel = nullptr;

    m_weddingExitChannel = AudioManager::GetInstance().PlayMusic(
        m_weddingExitSoundId);

    if (!m_weddingExitChannel) {
        spdlog::error("Wedding phase 3 music asset is unavailable; cancelling wedding mode");
        m_weddingModeActive = false;
        m_weddingPhase = 0;
        m_autoDuckingActive = false;
        m_autoTransitionToPhase2 = false;
        SetDuckFactor(1.0f);
        UpdateAllVolumes();
        return;
    }

    m_weddingModeActive = true;
    m_weddingPhase = 3;
    m_autoDuckingActive = true;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = transitionToNormalMusicAfter;

    if (!postWeddingPlaylist.empty()) {
        m_normalPlaylistAfterWedding = postWeddingPlaylist;
    }

    m_crossfadeDuration = 10.0f;

    UpdateAllVolumes();

    spdlog::info("Wedding phase 3 started with 10-second ducking in");
}

void UIManager::NextWeddingPhase() {
    if (!m_weddingModeActive) {
        StartWeddingPhase1(false);
        return;
    }

    if (m_weddingPhase == 1) {
        StartWeddingPhase2(m_transitionToNormalMusicAfterWedding);
    } else if (m_weddingPhase == 2) {
        StartWeddingPhase3(m_transitionToNormalMusicAfterWedding, m_normalPlaylistAfterWedding);
    } else if (m_weddingPhase == 3) {
        StopWeddingMode();
    }
}

const char* UIManager::GetWeddingStateString() const
{
    if (!m_weddingModeActive) return "idle";
    if (m_weddingPhase == 2) return "ceremony";
    if (m_weddingPhase == 3) return "exit";
    if (m_weddingPhase != 1) return "unknown";

    switch (m_phase1State)
    {
        case WeddingPhase1State::IDLE: return "entrance.idle";
        case WeddingPhase1State::FADING_OUT_PREVIOUS: return "entrance.fading_out_previous";
        case WeddingPhase1State::PLAYING_SFX_BEFORE: return "entrance.playing_sfx";
        case WeddingPhase1State::WAITING_AFTER_SFX: return "entrance.waiting_after_sfx";
        case WeddingPhase1State::DUCKING_IN: return "entrance.ducking_in";
        case WeddingPhase1State::PLAYING_ENTRANCE: return "entrance.playing";
        case WeddingPhase1State::DUCKING_OUT: return "entrance.ducking_out";
    }
    return "unknown";
}

void UIManager::StopWeddingMode()
{
    const auto stopChannel = [](FMOD::Channel*& channel) {
        if (!channel) return;
        bool playing = false;
        if (channel->isPlaying(&playing) == FMOD_OK && playing) channel->stop();
        channel = nullptr;
    };
    stopChannel(m_phase1SfxChannel);
    stopChannel(m_phase1EntranceChannel);
    stopChannel(m_weddingCeremonyChannel);
    stopChannel(m_weddingExitChannel);

    SetDuckFactor(1.0f);
    UpdateAllVolumes();

    m_weddingModeActive = false;
    m_weddingPhase = 0;
    m_autoDuckingActive = false;
    m_autoDuckStartFactor = 1.0f;
    m_autoDuckTimer = 0.0f;
    m_autoTransitionToPhase2 = false;
    m_transitionToNormalMusicAfterWedding = false;
    m_phase1State = WeddingPhase1State::IDLE;
    m_phase1DuckTimer = 0.0f;
}

void UIManager::StopAllMusic()
{
    PlaylistManager::GetInstance().Stop("");
    AudioManager::GetInstance().StopAllSoundsWithFadeOut();
    StopWeddingMode();

    spdlog::info("All music stopped.");
}

void UIManager::ResetSessionState()
{
    StopWeddingMode();
    m_weddingEntranceFilePath.clear();
    m_weddingCeremonyFilePath.clear();
    m_weddingExitFilePath.clear();
    m_normalPlaylistAfterWedding = "playlist_PostShow";
    m_playlistName.clear();
    m_playlistFeedback.clear();
    m_playlistFeedbackIsError = false;
    m_originalDuckFactor = 1.0f;
    m_targetDuckFactor = 0.3f;
    m_crossfadeDuration = 10.0f;
}

}

