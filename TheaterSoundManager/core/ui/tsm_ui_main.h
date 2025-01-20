// tsm_ui_main.h

#pragma once

#include "ui/tsm_ui_template.h"
#include "ui/tsm_ui_registry.h"

#include <fmod.hpp>
#include <fmod_errors.h>

#include <spdlog/spdlog.h>

#include "ui/tsm_ui_manager.h"
#include "tsm_file_utils.h"

#include <filesystem>
namespace fs = std::filesystem;

namespace TSM {

class UI_Main : public UI_Template {

public:
    UI_Main() = default;
    ~UI_Main() = default;

    UI_Main(const UI_Main&) = delete;
    UI_Main& operator=(const UI_Main&) = delete;

    void Render() override
    {
        static int selectedTrackIndex = -1;
        static bool useTimer = false;
        static Uint32 timerStartTime = 0;
        static int timerDurationMin = 1;

        auto &uiManager = TSM::UI_Manager::GetInstance();
        auto fmodSystem = uiManager.GetFmodSystem();
        auto playbackManager = uiManager.GetPlaybackManager();
        auto announcementManager = uiManager.GetAnnouncementManager();

        ImGui::Begin("Theater Sound Manager");

            if (ImGui::Button("Ajouter des musiques"))
            {
                std::vector<std::string> selectedFiles = OpenFileDialog();
                for (const auto& filepath : selectedFiles)
                {
                    std::string filename = fs::path(filepath).filename().string();
                    spdlog::info("Loading music file: {}", filename);

                    FMOD::Sound* sound = nullptr;
                    auto result = fmodSystem->createSound(filepath.c_str(), FMOD_DEFAULT | FMOD_CREATESTREAM, nullptr, &sound);
                    if (result == FMOD_OK && sound)
                    {
                        unsigned int lengthMs = 0;
                        sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);

                        playbackManager->addTrack(filepath, filename, sound, lengthMs);
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

            const auto& tracks = playbackManager->getTracks();
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
                        playbackManager->removeTrack(i);
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
                            std::swap(playbackManager->getTracksRef()[i], 
                                    playbackManager->getTracksRef()[i - 1]);

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
                            std::swap(playbackManager->getTracksRef()[i], 
                                    playbackManager->getTracksRef()[i + 1]);
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
                int curIdx = playbackManager->getCurrentTrackIndex();
                if (curIdx > 0) {
                    playbackManager->playTrack(curIdx - 1);
                    selectedTrackIndex = curIdx - 1;
                    if (useTimer) timerStartTime = SDL_GetTicks();
                }
            }

            ImGui::SameLine();
            if (ImGui::Button(playbackManager->isPlaying() ? "Pause" : "Jouer"))
            {
                if (playbackManager->isPlaying()) {
                    playbackManager->togglePause();
                }
                else {
                    if (selectedTrackIndex >= 0 && selectedTrackIndex < (int)tracks.size()) {
                        playbackManager->playTrack((size_t)selectedTrackIndex);
                        if (useTimer) timerStartTime = SDL_GetTicks();
                    }
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Stop"))
            {
                playbackManager->stopPlayback();
                timerStartTime = 0;
            }

            ImGui::SameLine();
            if (ImGui::Button("Suivant"))
            {
                if (playbackManager->isRandomOrder() && playbackManager->isRandomSegment()) {
                    size_t nextIdx = playbackManager->getNextTrackIndex();
                    playbackManager->playTrack(nextIdx);
                    selectedTrackIndex = (int)nextIdx;
                }
                else {
                    int curIdx = playbackManager->getCurrentTrackIndex();
                    if (curIdx >= 0 && (size_t)curIdx < tracks.size()) {
                        int nextIdx = curIdx + 1;
                        if (nextIdx >= (int)tracks.size()) {
                            nextIdx = 0;  
                        }
                        playbackManager->playTrack((size_t)nextIdx);
                        selectedTrackIndex = nextIdx;
                    }
                }
                if (useTimer) timerStartTime = SDL_GetTicks();
            }

            float volume = playbackManager->getVolume();
            if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f))
            {
                playbackManager->setVolume(volume);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Options de lecture :");

            bool randomOrder = playbackManager->isRandomOrder();
            if (ImGui::Checkbox("Lecture aléatoire", &randomOrder))
            {
                playbackManager->setRandomOrder(randomOrder);
            }

            bool randomSegment = playbackManager->isRandomSegment();
            if (ImGui::Checkbox("Segments aléatoires", &randomSegment))
            {
                playbackManager->setRandomSegment(randomSegment);
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
            
            int segmentDurationSec = playbackManager->getSegmentDuration() / 1000;
            if (ImGui::SliderInt("Durée du segment (secondes)", &segmentDurationSec, 10, 600)) {
                playbackManager->setSegmentDuration(segmentDurationSec * 1000);
            }
            
            int fadeDurationSec = playbackManager->getFadeDuration() / 1000;
            if (ImGui::SliderInt("Durée du fondu (secondes)", &fadeDurationSec, 1, 30)) {
                playbackManager->setFadeDuration(fadeDurationSec * 1000);
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (playbackManager->isActuallyPlaying() && playbackManager->getCurrentTrackIndex() >= 0) {
                const auto& allTracks = playbackManager->getTracks();
                int curIndex = playbackManager->getCurrentTrackIndex();
                if (curIndex < (int)allTracks.size()) {
                    const auto& currentTrack = allTracks[curIndex];
                    
                    ImGui::Text("Lecture en cours : %s", currentTrack.filename.c_str());

                    unsigned int position = playbackManager->getPosition();
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

                    Uint32 elapsedSegment = SDL_GetTicks() - playbackManager->getSegmentStartTime();
                    int segDur = playbackManager->getSegmentDuration();

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
    }

};

REGISTER_UI_TEMPLATE(UI_Main, "Main");

}