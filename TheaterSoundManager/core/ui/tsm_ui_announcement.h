// tsm_ui_announcement.h

#pragma once

#include "ui/tsm_ui_template.h"
#include "ui/tsm_ui_registry.h"

namespace TSM {

class UI_Announcement : public UI_Template {

public:
    UI_Announcement() = default;
    ~UI_Announcement() = default;

    UI_Announcement(const UI_Announcement&) = delete;
    UI_Announcement& operator=(const UI_Announcement&) = delete;

    void Render() override
    {
        auto &manager = UI_Manager::GetInstance();
        auto announcementManager = manager.GetAnnouncementManager();
        auto playbackManager = manager.GetPlaybackManager();

        ImGui::Begin("Annonces");

                float duckFactor = announcementManager->getMusicDuckFactor();
                if (ImGui::SliderFloat("Duck Factor", &duckFactor, 0.f, 1.f)) {
                    announcementManager->setMusicDuckFactor(duckFactor);
                }

                float volumeMultiplier = announcementManager->getAnnouncementVolumeMultiplier();
                if (ImGui::SliderFloat("Multiplier Annonce", &volumeMultiplier, 0.5f, 5.f)) {
                    announcementManager->setAnnouncementVolumeMultiplier(volumeMultiplier);
                }

                ImGui::Separator();

                bool useStartSfx = announcementManager->getUseStartSfx();
                if (ImGui::Checkbox("Utiliser SFX début", &useStartSfx)) {
                    announcementManager->setUseStartSfx(useStartSfx);
                }

                if (ImGui::Button("Charger SFX début")) {
                    auto files = OpenFileDialog("Audio Files\0*.mp3;*.wav\0All\0*.*\0");
                    if (!files.empty()) {
                        announcementManager->loadStartSfx(files[0]);
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Supprimer SFX début")) {
                    announcementManager->releaseStartSfx();
                }

                ImGui::SameLine();
                if (announcementManager->hasStartSfx()) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", announcementManager->getStartSfxFilename().c_str());
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Aucun SFX de début chargé");
                }

                bool useEndSfx = announcementManager->getUseEndSfx();
                if (ImGui::Checkbox("Utiliser SFX fin", &useEndSfx)) {
                    announcementManager->setUseEndSfx(useEndSfx);
                }

                if (ImGui::Button("Charger SFX fin")) {
                    auto files = OpenFileDialog("Audio Files\0*.mp3;*.wav\0All\0*.*\0");
                    if (!files.empty()) {
                        announcementManager->loadEndSfx(files[0]);
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button("Supprimer SFX fin")) {
                    announcementManager->releaseEndSfx();
                }

                ImGui::SameLine();
                if (announcementManager->hasEndSfx()) {
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", announcementManager->getEndSfxFilename().c_str());
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Aucun SFX de fin chargé");
                }

                float sfxVol = announcementManager->getSfxVolume();
                if (ImGui::SliderFloat("Volume SFX", &sfxVol, 0.0f, 2.0f)) {
                    announcementManager->setSfxVolume(sfxVol);
                }

                ImGui::Separator();

                if (ImGui::Button("Ajouter une annonce")) {
                    auto files = OpenFileDialog("Audio Files\0*.mp3;*.wav\0All\0*.*\0");
                    for(auto& f : files) {
                        announcementManager->loadAnnouncement(f);
                    }
                }

                if (ImGui::BeginTable("AnnTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Actif", ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("Heure", ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, 60.f);
                    ImGui::TableSetupColumn("Fichier", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Test", ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("Suppr", ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableHeadersRow();

                    int idx = 0;
                    bool needsRemoval = false;
                    size_t indexToRemove = 0;

                    announcementManager->forEachAnnouncement([&](AnnouncementManager::Announcement& ann) {
                        ImGui::TableNextRow();

                        ImGui::TableSetColumnIndex(0);
                        bool enabled = ann.isEnabled;
                        if (ImGui::Checkbox(("##enabled" + std::to_string(idx)).c_str(), &enabled)) {
                            ann.isEnabled = enabled;
                        }

                        ImGui::TableSetColumnIndex(1);
                        int hour = ann.hour;
                        if (ImGui::InputInt(("##hour" + std::to_string(idx)).c_str(), &hour)) {
                            ann.hour = std::clamp(hour, 0, 23);
                        }

                        ImGui::TableSetColumnIndex(2);
                        int minute = ann.minute;
                        if (ImGui::InputInt(("##min" + std::to_string(idx)).c_str(), &minute)) {
                            ann.minute = std::clamp(minute, 0, 59);
                        }

                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemWidth(-1);
                        float vol = ann.volume;
                        if (ImGui::SliderFloat(("##vol" + std::to_string(idx)).c_str(), &vol, 0.f, 2.f)) {
                            ann.volume = vol;
                        }

                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(ann.filename.c_str());

                        ImGui::TableSetColumnIndex(5);
                        if (ImGui::Button(("T##" + std::to_string(idx)).c_str())) {
                            announcementManager->testAnnouncement(idx, *playbackManager);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Tester l'annonce");
                        }

                        ImGui::TableSetColumnIndex(6);
                        if (ImGui::Button(("X##" + std::to_string(idx)).c_str())) {
                            needsRemoval = true;
                            indexToRemove = idx;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("Supprimer l'annonce");
                        }

                        idx++;
                    });

                    if (needsRemoval) {
                        announcementManager->removeAnnouncement(indexToRemove);
                    }

                    ImGui::EndTable();
                }

                ImGui::TextWrapped("Les annonces se déclenchent automatiquement à l'heure indiquée.");
                ImGui::End();
    }

};

REGISTER_UI_TEMPLATE(UI_Announcement, "Announcement");

}