// tsm_ui_manager.cpp

#include <SDL.h>
#include <SDL_events.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include "ui/tsm_ui_manager.h"
#include "ui/tsm_ui_registry.h"

#include <fmod.hpp>
#include <fmod_errors.h>

#include <spdlog/spdlog.h>

namespace TSM {

auto glsl_version = "#version 460";

void ERRCHECK(FMOD_RESULT result)
{
    if (result != FMOD_OK)
    {
        spdlog::error("FMOD error {}: {}", static_cast<int>(result), FMOD_ErrorString(result));
    }
}

UI_Manager::UI_Manager()
    : m_window(nullptr)
    , m_gl_context(nullptr)
    , m_fmodSystem(nullptr)
    , m_fmodResult(FMOD_OK)
    , m_playbackManager(nullptr)
    , m_announcementManager(nullptr)
    , m_isRunning(false)
{
}

UI_Manager::~UI_Manager() 
{
    Shutdown();
}

bool UI_Manager::Initialize(const std::string& title, int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        spdlog::error("SDL_Init Error: {}", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    m_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        window_flags
    );

    if (!m_window) {
        spdlog::error("Failed to create SDL window: {}", SDL_GetError());
        return false;
    }

    // Create OpenGL context
    m_gl_context = SDL_GL_CreateContext(m_window);
    if (!m_gl_context) {
        spdlog::error("Failed to create OpenGL context: {}", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(m_window, m_gl_context);
    SDL_GL_SetSwapInterval(1); 

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::SetCurrentContext(ImGui::GetCurrentContext());


    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

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
    }

    ImGui_ImplSDL2_InitForOpenGL(m_window, m_gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    io.Fonts->AddFontFromFileTTF("assets/fonts/Jost-Regular.ttf", 24.0f);

    m_fmodResult = FMOD::System_Create(&m_fmodSystem);
    if (m_fmodResult != FMOD_OK) {
        spdlog::error("Failed to create FMOD system: {}", FMOD_ErrorString(m_fmodResult));
        return false;
    }

    m_fmodResult = m_fmodSystem->init(512, FMOD_INIT_NORMAL, nullptr);
    if (m_fmodResult != FMOD_OK) {
        spdlog::error("Failed to initialize FMOD system: {}", FMOD_ErrorString(m_fmodResult));
        return false;
    }

    m_playbackManager = new PlaybackManager(m_fmodSystem);
    m_announcementManager = new AnnouncementManager(m_fmodSystem);

    m_isRunning = true;

    return true;
}

void UI_Manager::Shutdown()
{
    if (m_announcementManager) {
        m_announcementManager->prepareForShutdown();
    }

    if (m_playbackManager) {
        m_playbackManager->stopPlayback();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (m_announcementManager) {
        delete m_announcementManager;
        m_announcementManager = nullptr;
    }

    if (m_playbackManager) {
        delete m_playbackManager;
        m_playbackManager = nullptr;
    }

    if (m_fmodSystem) {
        m_fmodSystem->update();
        SDL_Delay(100);
    }

    if (m_fmodSystem) {
        m_fmodSystem->close();
        m_fmodSystem->release();
        m_fmodSystem = nullptr;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (m_gl_context) {
        SDL_GL_DeleteContext(m_gl_context);
        m_gl_context = nullptr;
    }

    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
}

void UI_Manager::ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        
        switch (event.type) {
            case SDL_QUIT:
                m_isRunning = false;
                break;
                
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE && 
                    event.window.windowID == SDL_GetWindowID(m_window)) {
                    m_isRunning = false;
                }
                break;
        }
    }
}

void UI_Manager::SetupMayaStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        const ImVec4 mayaBackground = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
        const ImVec4 mayaPanel = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        const ImVec4 mayaAccent = ImVec4(0.40f, 0.75f, 0.50f, 1.00f); 
        const ImVec4 mayaText = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
        
        // Base elements
        colors[ImGuiCol_Text] = mayaText;
        colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
        colors[ImGuiCol_WindowBg] = mayaBackground;
        colors[ImGuiCol_ChildBg] = mayaBackground;
        colors[ImGuiCol_PopupBg] = mayaBackground;
        colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
        
        // Frame and interactive elements
        colors[ImGuiCol_FrameBg] = mayaPanel;
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = mayaAccent;
        
        // Title bar
        colors[ImGuiCol_TitleBg] = mayaBackground;
        colors[ImGuiCol_TitleBgActive] = mayaPanel;
        colors[ImGuiCol_TitleBgCollapsed] = mayaBackground;
        
        // Menu bar
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
        
        // Scrollbar
        colors[ImGuiCol_ScrollbarBg] = mayaBackground;
        colors[ImGuiCol_ScrollbarGrab] = mayaPanel;
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive] = mayaAccent;
        
        // Interactive elements
        colors[ImGuiCol_CheckMark] = mayaAccent;
        colors[ImGuiCol_SliderGrab] = mayaAccent;
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.80f, 0.55f, 1.00f);
        colors[ImGuiCol_Button] = mayaPanel;
        colors[ImGuiCol_ButtonHovered] = mayaAccent;
        colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.80f, 0.55f, 1.00f);
        
        // Headers
        colors[ImGuiCol_Header] = mayaPanel;
        colors[ImGuiCol_HeaderHovered] = mayaAccent;
        colors[ImGuiCol_HeaderActive] = ImVec4(0.45f, 0.80f, 0.55f, 1.00f);
        
        // Separators
        colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_SeparatorHovered] = mayaAccent;
        colors[ImGuiCol_SeparatorActive] = mayaAccent;
        
        // Resize grips
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.20f, 0.20f, 0.20f, 0.50f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.40f, 0.40f, 0.40f, 0.60f);
        colors[ImGuiCol_ResizeGripActive] = mayaAccent;
        
        // Tabs
        colors[ImGuiCol_Tab] = mayaBackground;
        colors[ImGuiCol_TabHovered] = mayaAccent;
        colors[ImGuiCol_TabSelected] = mayaPanel;
        colors[ImGuiCol_TabSelectedOverline] = mayaAccent;
        colors[ImGuiCol_TabDimmed] = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = mayaPanel;
        colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.35f, 0.65f, 0.45f, 1.00f);
        
        // Docking
        colors[ImGuiCol_DockingPreview] = mayaAccent;
        colors[ImGuiCol_DockingEmptyBg] = mayaBackground;
        
        // Plots
        colors[ImGuiCol_PlotLines] = mayaText;
        colors[ImGuiCol_PlotLinesHovered] = mayaAccent;
        colors[ImGuiCol_PlotHistogram] = mayaText;
        colors[ImGuiCol_PlotHistogramHovered] = mayaAccent;
        
        // Tables
        colors[ImGuiCol_TableHeaderBg] = mayaPanel;
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
        
        // Links and selection
        colors[ImGuiCol_TextLink] = ImVec4(0.50f, 0.85f, 0.60f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.40f, 0.75f, 0.50f, 0.35f);
        
        // Misc
        colors[ImGuiCol_DragDropTarget] = mayaAccent;
        colors[ImGuiCol_NavCursor] = mayaAccent;
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.80f, 0.80f, 0.80f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.15f, 0.15f, 0.15f, 0.35f);
        
        // Style
        style.WindowRounding = 0.0f;
        style.ChildRounding = 0.0f;
        style.FrameRounding = 2.0f;
        style.PopupRounding = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding = 2.0f;
        style.TabRounding = 2.0f;
}

void UI_Manager::SetupBlenderStyle()
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

void UI_Manager::SetupHoudiniStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;

        // Base colors - Pastel palette
        const ImVec4 houdiniBackground = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
        const ImVec4 houdiniPanel = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
        const ImVec4 houdiniAccent = ImVec4(0.95f, 0.45f, 0.35f, 1.00f);      // Soft red
        const ImVec4 houdiniAccentBright = ImVec4(1.00f, 0.55f, 0.45f, 1.00f); // Brighter pastel red
        const ImVec4 houdiniAccentDark = ImVec4(0.80f, 0.35f, 0.25f, 1.00f);   // Darker pastel red
        const ImVec4 houdiniText = ImVec4(0.85f, 0.85f, 0.85f, 1.00f);

        // Base elements
        colors[ImGuiCol_Text] = houdiniText;
        colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.55f, 1.00f);
        colors[ImGuiCol_WindowBg] = houdiniBackground;
        colors[ImGuiCol_ChildBg] = houdiniBackground;
        colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.14f, 0.14f, 0.98f);
        colors[ImGuiCol_Border] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.10f, 0.10f, 0.10f, 0.00f);

        // Frame and interactive elements
        colors[ImGuiCol_FrameBg] = houdiniPanel;
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.95f, 0.45f, 0.35f, 0.25f);
        colors[ImGuiCol_FrameBgActive] = houdiniAccent;

        // Title bar with softer colors
        colors[ImGuiCol_TitleBg] = houdiniBackground;
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.14f, 0.14f, 0.14f, 0.90f);

        // Menu bar
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);

        // Scrollbar with pastel accents
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.14f, 0.14f, 0.14f, 0.80f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = houdiniAccentDark;
        colors[ImGuiCol_ScrollbarGrabActive] = houdiniAccent;

        // Interactive elements with pastel accents
        colors[ImGuiCol_CheckMark] = houdiniAccentBright;
        colors[ImGuiCol_SliderGrab] = houdiniAccent;
        colors[ImGuiCol_SliderGrabActive] = houdiniAccentBright;
        colors[ImGuiCol_Button] = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = houdiniAccent;
        colors[ImGuiCol_ButtonActive] = houdiniAccentBright;

        // Headers with soft red accents
        colors[ImGuiCol_Header] = ImVec4(0.95f, 0.45f, 0.35f, 0.25f);
        colors[ImGuiCol_HeaderHovered] = houdiniAccent;
        colors[ImGuiCol_HeaderActive] = houdiniAccentBright;

        // Separator
        colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
        colors[ImGuiCol_SeparatorHovered] = houdiniAccent;
        colors[ImGuiCol_SeparatorActive] = houdiniAccentBright;

        // Resize grip
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.95f, 0.45f, 0.35f, 0.20f);
        colors[ImGuiCol_ResizeGripHovered] = houdiniAccent;
        colors[ImGuiCol_ResizeGripActive] = houdiniAccentBright;

        // Tabs with pastel accents
        colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
        colors[ImGuiCol_TabHovered] = houdiniAccent;
        colors[ImGuiCol_TabSelected] = ImVec4(0.95f, 0.45f, 0.35f, 0.40f);
        colors[ImGuiCol_TabSelectedOverline] = houdiniAccentBright;
        colors[ImGuiCol_TabDimmed] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.95f, 0.45f, 0.35f, 0.25f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = houdiniAccentDark;

        // Docking
        colors[ImGuiCol_DockingPreview] = ImVec4(0.95f, 0.45f, 0.35f, 0.50f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);

        // Plot elements
        colors[ImGuiCol_PlotLines] = ImVec4(0.95f, 0.45f, 0.35f, 0.80f);
        colors[ImGuiCol_PlotLinesHovered] = houdiniAccentBright;
        colors[ImGuiCol_PlotHistogram] = ImVec4(0.95f, 0.45f, 0.35f, 0.80f);
        colors[ImGuiCol_PlotHistogramHovered] = houdiniAccentBright;

        // Tables
        colors[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
        colors[ImGuiCol_TableBorderStrong] = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
        colors[ImGuiCol_TableBorderLight] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
        colors[ImGuiCol_TableRowBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);

        // Text and selection
        colors[ImGuiCol_TextLink] = ImVec4(1.00f, 0.50f, 0.40f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.95f, 0.45f, 0.35f, 0.35f);

        // Navigation and modal
        colors[ImGuiCol_DragDropTarget] = houdiniAccentBright;
        colors[ImGuiCol_NavCursor] = houdiniAccent;
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.95f, 0.45f, 0.35f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.14f, 0.14f, 0.14f, 0.50f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.14f, 0.14f, 0.14f, 0.40f);

        style.WindowRounding = 0.0f;      
        style.ChildRounding = 0.0f;
        style.FrameRounding = 2.0f;     
        style.PopupRounding = 0.0f;
        style.ScrollbarRounding = 2.0f;
        style.GrabRounding = 2.0f;
        style.TabRounding = 2.0f;

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

void UI_Manager::PreRender()
{
    // Skip rendering if window is minimized
    if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(10);
        return;
    }

    // Update managers
    if (m_playbackManager) {
        m_playbackManager->update();
    }

    if (m_fmodSystem) {
        m_fmodResult = m_fmodSystem->update();
        if (m_fmodResult != FMOD_OK) {
            spdlog::error("FMOD update error: {}", FMOD_ErrorString(m_fmodResult));
        }
    }

    if (m_announcementManager && m_playbackManager) {
        m_announcementManager->checkAndPlayAnnouncements(*m_playbackManager);
    }

    // Start new ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    SetupDockSpace();
}

void UI_Manager::Render()
{
    UI_Registry::Instance().renderAll();
}

void UI_Manager::PostRender()
{
    ImGui::Render();

    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    
    ImVec4 clear_color = ImVec4(0.1f, 0.105f, 0.11f, 1.00f);
    glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
    }

    SDL_GL_SwapWindow(m_window);
}

void UI_Manager::Run()
{
    while (m_isRunning) {
        ProcessEvents();
        PreRender();
        Render();
        PostRender();
    }
}

void UI_Manager::SetupDockSpace()
{
    static bool opt_fullscreen = true;
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    if (opt_fullscreen)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
        window_flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar();

    if (opt_fullscreen)
        ImGui::PopStyleVar(2);

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
    {
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New", "Ctrl+N")) {}
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {}
            if (ImGui::MenuItem("Save", "Ctrl+S")) {}
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {}
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) 
            {
                SDL_Event quit_event;
                quit_event.type = SDL_QUIT;
                SDL_PushEvent(&quit_event);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Reset Layout")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About")) {}
            ImGui::EndMenu();
        }

        float windowWidth = ImGui::GetWindowWidth();
        float menuBarHeight = ImGui::GetFrameHeight();
        float buttonWidth = 46.0f;
        
        float menuWidth = 0;
        menuWidth = ImGui::GetCursorPosX(); 

        float dragAreaWidth = windowWidth - menuWidth - (3 * buttonWidth);
        ImGui::SetCursorPosX(menuWidth);
        
        ImGui::InvisibleButton("##draggable", ImVec2(dragAreaWidth, menuBarHeight));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0))
        {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            SDL_Window* window = m_window;
            int x, y;
            SDL_GetWindowPosition(window, &x, &y);
            SDL_SetWindowPosition(window, x + delta.x, y + delta.y);
        }
        ImGui::EndMenuBar();
    }

    ImGui::End();
}

}