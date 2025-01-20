// tsm_imgui.h

#pragma once

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

const char* glsl_version = "#version 460";

namespace tsm
{

void init_imgui(SDL_Window* window, SDL_GLContext gl_context)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableSetMousePos;
	io.ConfigDockingWithShift = true;

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 5.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding = 5.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.105f, 0.11f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.205f, 0.21f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.3f, 0.305f, 0.31f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.39f, 0.4f, 0.41f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.15f, 0.1505f, 0.151f, 1.0f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(1.0f, 0.6f, 0.7f, 1.0f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 0.6f, 0.7f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.35f, 0.63f, 0.39f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.35f, 0.63f, 0.39f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.35f, 0.63f, 0.39f, 1.0f);

    ImVec4 dominantColor = ImVec4(0.5f, 0.7f, 0.9f, 1.0f);
    colors[ImGuiCol_SliderGrab] = dominantColor;
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.4f, 0.6f, 0.8f, 1.0f);
    colors[ImGuiCol_CheckMark] = dominantColor;
    colors[ImGuiCol_TextSelectedBg] = dominantColor;
    colors[ImGuiCol_PlotLines] = dominantColor;
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.4f, 0.6f, 0.8f, 1.0f);

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 5.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);
}

}

