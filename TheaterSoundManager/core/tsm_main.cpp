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

namespace fs = std::filesystem;

Uint32 pauseStartTime = 0;
Uint32 totalPausedTime = 0;

std::vector<std::string> OpenFileDialog(const char* filter = "Audio Files\0*.mp3;*.wav;*.ogg;*.flac\0All Files\0*.*\0") {
    std::vector<std::string> filenames;

    OPENFILENAMEA ofn;       
    CHAR szFile[8192] = { 0 }; 
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL; 
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    ofn.lpstrDefExt = "mp3";

    if (GetOpenFileNameA(&ofn)) {
        char* p = szFile;
        std::string directory = p;
        p += directory.length() + 1;

        if (*p == '\0') {
            filenames.push_back(directory);
        }
        else {
            while (*p) {
                std::string filename = p;
                p += filename.length() + 1;
                filenames.push_back(directory + "\\" + filename);
            }
        }
    }

    return filenames;
}

bool init_sdl(SDL_Window** window, SDL_GLContext* gl_context, const char* title, int width, int height)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
    {
        std::cerr << "Erreur SDL_Init: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    *window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, window_flags);
    if (*window == nullptr)
    {
        std::cerr << "Erreur SDL_CreateWindow: " << SDL_GetError() << std::endl;
        return false;
    }

    *gl_context = SDL_GL_CreateContext(*window);
    SDL_GL_MakeCurrent(*window, *gl_context);
    SDL_GL_SetSwapInterval(1);

    if (*gl_context == nullptr)
    {
        std::cerr << "Erreur SDL_GL_CreateContext: " << SDL_GetError() << std::endl;
        return false;
    }

    return true;
}

void cleanup_sdl(SDL_Window* window, SDL_GLContext gl_context)
{
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

void ERRCHECK(FMOD_RESULT result)
{
    if (result != FMOD_OK)
    {
        spdlog::error("FMOD error {}: {}", static_cast<int>(result), FMOD_ErrorString(result));
    }
}


int main(int argc, char** argv)
{
    tsm::Logger::init();
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;

    if (!init_sdl(&window, &gl_context, "Theater Sound Manager", 1280, 720)) {
        spdlog::error("Failed to initialize SDL");
        return -1;
    }

    FMOD::System* fmodSystem = nullptr;
    FMOD_RESULT result = FMOD::System_Create(&fmodSystem);
    if (result != FMOD_OK) {
        spdlog::error("FMOD error {}: {}", static_cast<int>(result), FMOD_ErrorString(result));
    }
    result = fmodSystem->init(512, FMOD_INIT_NORMAL, nullptr);
    if (result != FMOD_OK) {
        spdlog::error("FMOD error {}: {}", static_cast<int>(result), FMOD_ErrorString(result));
    }


    tsm::init_imgui(window, gl_context);

    {
        PlaybackManager playbackManager(fmodSystem);
        AnnouncementManager announcementManager(fmodSystem);

        static int selectedTrackIndex = -1;

        bool useTimer = false;
        int  timerDurationMin = 10;
        Uint32 timerStartTime = 0;
        
        bool running = true;
        ImVec4 clear_color = ImVec4(0.1f, 0.105f, 0.11f, 1.00f);

        while (running)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT)
                    running = false;
                if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && 
                    event.window.windowID == SDL_GetWindowID(window))
                    running = false;
            }

            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
            {
                SDL_Delay(10);
                continue;
            }

            playbackManager.update();
            result = fmodSystem->update();
            if (result != FMOD_OK) {
                spdlog::error("FMOD error {}: {}", static_cast<int>(result), FMOD_ErrorString(result));
            }
            
            announcementManager.checkAndPlayAnnouncements(playbackManager);

            if (useTimer && timerStartTime > 0)
            {
                Uint32 elapsedTimer = SDL_GetTicks() - timerStartTime;
                if (elapsedTimer >= (Uint32)(timerDurationMin * 60000))
                {
                    playbackManager.stopPlayback();
                    timerStartTime = 0;
                }
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            const ImGuiWindowFlags dockspace_flags = ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_AlwaysAutoResize;

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

            if (ImGui::Begin("MainDockSpace", nullptr, dockspace_flags))
            {
                ImGui::End();
            }
            ImGui::PopStyleVar(3);

            ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");   
            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

            ImGui::Begin("Theater Sound Manager");

            if (ImGui::Button("Ajouter des musiques"))
            {
                std::vector<std::string> selectedFiles = OpenFileDialog();
                for (const auto& filepath : selectedFiles)
                {
                    std::string filename = fs::path(filepath).filename().string();
                    spdlog::info("Loading music file: {}", filename);

                    FMOD::Sound* sound = nullptr;
                    result = fmodSystem->createSound(filepath.c_str(), FMOD_DEFAULT | FMOD_CREATESTREAM, nullptr, &sound);
                    if (result == FMOD_OK && sound)
                    {
                        unsigned int lengthMs = 0;
                        sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);

                        playbackManager.addTrack(filepath, filename, sound, lengthMs);
                        spdlog::info("Successfully loaded: %s (length = %u ms)", filename.c_str(), lengthMs);
                    }
                    else
                    {
                        spdlog::error("Failed to load music file: {} (FMOD error: {})", filename, FMOD_ErrorString(result));
                    }
                }
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Playlist :");

            const auto& tracks = playbackManager.getTracks();
            ImGui::BeginChild("PlaylistChild", ImVec2(0, 250), true);

            if (ImGui::BeginTable("PlaylistTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Sel",   ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("Titre", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Durée", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < tracks.size(); ++i)
                {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(static_cast<int>(i));
                    {
                        bool isSelected = (selectedTrackIndex == (int)i);
                        if (ImGui::RadioButton("##sel", isSelected))
                        {
                            selectedTrackIndex = (int)i;
                        }
                    }
                    ImGui::PopID();

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(tracks[i].filename.c_str());

                    ImGui::TableSetColumnIndex(2);
                    {
                        unsigned int totalMs = tracks[i].lengthMs;
                        int totalSec = totalMs / 1000;
                        int min = totalSec / 60;
                        int sec = totalSec % 60;
                        ImGui::Text("%02d:%02d", min, sec);
                    }

                    ImGui::TableSetColumnIndex(3);

                    ImGui::PushID((int)i * 10 + 1);
                    if (ImGui::Button("Suppr"))
                    {
                        playbackManager.removeTrack(i);
                        if (selectedTrackIndex == (int)i) {
                            selectedTrackIndex = -1;
                        }
                        if (selectedTrackIndex > (int)i) {
                            selectedTrackIndex--;
                        }
                        --i; 
                        ImGui::PopID();
                        break; 
                    }
                    ImGui::PopID();

                    ImGui::SameLine();

                    if (ImGui::ArrowButton(("Up##" + std::to_string(i)).c_str(), ImGuiDir_Up)) {
                        if (i > 0) {
                            std::swap(playbackManager.getTracksRef()[i], 
                                    playbackManager.getTracksRef()[i - 1]);
                        
                            if (selectedTrackIndex == (int)i) {
                                selectedTrackIndex = (int)i - 1;
                            } else if (selectedTrackIndex == (int)i - 1) {
                                selectedTrackIndex = (int)i;
                            }
                        }
                    }
                    ImGui::SameLine();

                    if (ImGui::ArrowButton(("Down##" + std::to_string(i)).c_str(), ImGuiDir_Down)) {
                        if (i < tracks.size() - 1) {
                            std::swap(playbackManager.getTracksRef()[i], 
                                    playbackManager.getTracksRef()[i + 1]);
                            if (selectedTrackIndex == (int)i) {
                                selectedTrackIndex = (int)i + 1;
                            } else if (selectedTrackIndex == (int)i + 1) {
                                selectedTrackIndex = (int)i;
                            }
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Contrôles de lecture :");

            if (ImGui::Button("Précédent"))
            {
                int curIdx = playbackManager.getCurrentTrackIndex();
                if (curIdx > 0) {
                    playbackManager.playTrack(curIdx - 1);
                    selectedTrackIndex = curIdx - 1;
                    if (useTimer) timerStartTime = SDL_GetTicks();
                }
            }

            ImGui::SameLine();
            if (ImGui::Button(playbackManager.isPlaying() ? "Pause" : "Jouer"))
            {
                if (playbackManager.isPlaying()) {
                    playbackManager.togglePause();
                }
                else {
                    if (selectedTrackIndex >= 0 && selectedTrackIndex < (int)tracks.size()) {
                        playbackManager.playTrack((size_t)selectedTrackIndex);
                        if (useTimer) timerStartTime = SDL_GetTicks();
                    }
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Stop"))
            {
                playbackManager.stopPlayback();
                timerStartTime = 0;
            }

            ImGui::SameLine();
            if (ImGui::Button("Suivant"))
            {
                if (playbackManager.isRandomOrder() && playbackManager.isRandomSegment()) {
                    size_t nextIdx = playbackManager.getNextTrackIndex();
                    playbackManager.playTrack(nextIdx);
                    selectedTrackIndex = (int)nextIdx;
                }
                else {
                    int curIdx = playbackManager.getCurrentTrackIndex();
                    if (curIdx >= 0 && (size_t)curIdx < tracks.size()) {
                        int nextIdx = curIdx + 1;
                        if (nextIdx >= (int)tracks.size()) {
                            nextIdx = 0;  
                        }
                        playbackManager.playTrack((size_t)nextIdx);
                        selectedTrackIndex = nextIdx;
                    }
                }
                if (useTimer) timerStartTime = SDL_GetTicks();
            }

            float volume = playbackManager.getVolume();
            if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f))
            {
                playbackManager.setVolume(volume);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Options de lecture :");

            bool randomOrder = playbackManager.isRandomOrder();
            if (ImGui::Checkbox("Lecture aléatoire", &randomOrder))
            {
                playbackManager.setRandomOrder(randomOrder);
            }

            bool randomSegment = playbackManager.isRandomSegment();
            if (ImGui::Checkbox("Segments aléatoires", &randomSegment))
            {
                playbackManager.setRandomSegment(randomSegment);
            }

            if (randomOrder && randomSegment) {
                ImGui::TextColored(ImVec4(1,0.7f,0.2f,1), 
                  "ATTENTION: Lecture aléatoire + segments aléatoires = navigation \"Précédent/Suivant\" limitée.");
            }

            ImGui::Checkbox("Utiliser le timer", &useTimer);
            if (useTimer)
            {
                ImGui::SliderInt("Durée du timer (minutes)", &timerDurationMin, 1, 120);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Paramètres de segmentation :");
            
            int segmentDurationSec = playbackManager.getSegmentDuration() / 1000;
            if (ImGui::SliderInt("Durée du segment (secondes)", &segmentDurationSec, 10, 600)) {
                playbackManager.setSegmentDuration(segmentDurationSec * 1000);
            }
            
            int fadeDurationSec = playbackManager.getFadeDuration() / 1000;
            if (ImGui::SliderInt("Durée du fondu (secondes)", &fadeDurationSec, 1, 30)) {
                playbackManager.setFadeDuration(fadeDurationSec * 1000);
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (playbackManager.isActuallyPlaying() && playbackManager.getCurrentTrackIndex() >= 0) {
                const auto& allTracks = playbackManager.getTracks();
                int curIndex = playbackManager.getCurrentTrackIndex();
                if (curIndex < (int)allTracks.size()) {
                    const auto& currentTrack = allTracks[curIndex];
                    
                    ImGui::Text("Lecture en cours : %s", currentTrack.filename.c_str());

                    unsigned int position = playbackManager.getPosition();
                    unsigned int length = currentTrack.lengthMs;
                    
                    float progress = (length > 0) ? (float)position / (float)length : 0.0f;
                    ImGui::ProgressBar(progress, ImVec2(-1, 0));

                    int minutes = position / (1000 * 60);
                    int seconds = (position / 1000) % 60;
                    ImGui::Text("Position : %d:%02d", minutes, seconds);
                    ImGui::SameLine();
                    int remainMs = (int)length - (int)position;
                    if (remainMs < 0) remainMs = 0;
                    minutes = remainMs / (1000 * 60);
                    seconds = (remainMs / 1000) % 60;
                    ImGui::Text("/ Reste %d:%02d", minutes, seconds);

                    Uint32 elapsedSegment = SDL_GetTicks() - playbackManager.getSegmentStartTime();
                    int segDur = playbackManager.getSegmentDuration();

                    if (segDur > 0 && (unsigned int)segDur <= currentTrack.lengthMs) {
                        if (elapsedSegment <= (Uint32)segDur) {
                            float segmentProgress = (float)elapsedSegment / (float)segDur;
                            segmentProgress = std::min(segmentProgress, 1.0f);
                            
                            ImGui::Text("Progression du segment :");
                            ImGui::ProgressBar(segmentProgress, ImVec2(-1, 0));

                            int remainingSeg = ((int)segDur - (int)elapsedSegment) / 1000;
                            if (remainingSeg < 0) remainingSeg = 0;
                            ImGui::Text("Temps restant sur le segment : %d:%02d", 
                                remainingSeg/60, remainingSeg%60);
                        }
                    }
                }
            } else {
                ImGui::Text("Aucune lecture en cours");
            }

            ImGui::End();

            ImGui::Begin("Annonces");

            ImGui::Text("Réglages Annonces :");
            float duckFactor = announcementManager.getMusicDuckFactor();
            if (ImGui::SliderFloat("Duck Factor", &duckFactor, 0.f, 1.f, "%.2f")) {
                announcementManager.setMusicDuckFactor(duckFactor);
            }

            float volumeMultiplier = announcementManager.getAnnouncementVolumeMultiplier();
            if (ImGui::SliderFloat("Multiplier Annonce", &volumeMultiplier, 0.5f, 5.f, "%.2fx")) {
                announcementManager.setAnnouncementVolumeMultiplier(volumeMultiplier);
            }

            if (ImGui::Button("Ajouter une annonce")) {
                auto files = OpenFileDialog("Audio Files\0*.mp3;*.wav\0All\0*.*\0");
                for(auto& f : files) {
                    announcementManager.loadAnnouncement(f);
                }
            }
            ImGui::Separator();

            if (ImGui::BeginTable("AnnTable", 6, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Actif",   ImGuiTableColumnFlags_WidthFixed, 40.f);
                ImGui::TableSetupColumn("Heure",   ImGuiTableColumnFlags_WidthFixed, 40.f);
                ImGui::TableSetupColumn("Minute",  ImGuiTableColumnFlags_WidthFixed, 40.f);
                ImGui::TableSetupColumn("Volume",  ImGuiTableColumnFlags_WidthFixed, 60.f);
                ImGui::TableSetupColumn("Fichier", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Test Audio", ImGuiTableColumnFlags_WidthFixed, 60.f);
                ImGui::TableHeadersRow();

                {
                    int i = 0; 
                    announcementManager.forEachAnnouncement([&](AnnouncementManager::Announcement& ann) {
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Checkbox(std::string("##enabled"+std::to_string(i)).c_str(), &ann.isEnabled);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(i*100+1);
                        ImGui::InputInt("##hour", &ann.hour, 1);
                        ImGui::PopID();

                        ImGui::TableSetColumnIndex(2);
                        ImGui::PushID(i*100+2);
                        ImGui::InputInt("##min", &ann.minute, 1);
                        ImGui::PopID();

                        ImGui::TableSetColumnIndex(3);
                        ImGui::PushID(i*100+3);
                        ImGui::SliderFloat("##vol", &ann.volume, 0.f, 2.f, "%.2f");
                        ImGui::PopID();

                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(ann.filename.c_str());

                        ImGui::TableSetColumnIndex(5);
                        ImGui::PushID(i*100+4);
                        if (ImGui::Button("Test")) {
                            announcementManager.testAnnouncement(i, playbackManager);
                        }
                        ImGui::PopID();
                        
                        i++;
                    });
                }
                ImGui::EndTable();
            }

            ImGui::TextWrapped("Les annonces se déclenchent automatiquement à l'heure / minute indiquées.");

            ImGui::End();

            ImGui::Render();
            ImGuiIO& io = ImGui::GetIO();
            glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
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

            SDL_GL_SwapWindow(window);
        }
    }
    

    spdlog::info("Cleaning up announcements...");

    if(fmodSystem) {
        fmodSystem->close();
        fmodSystem->release();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    cleanup_sdl(window, gl_context);

    return 0;
}
