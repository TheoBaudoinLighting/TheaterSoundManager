// tsm_main.cpp

#include <chrono>
#include <string>

#include "tsm_announcement_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_bluetooth_server.h"
#include "tsm_config.h"
#include "tsm_fmod_wrapper.h"
#include "tsm_logger.h"
#include "tsm_playlist_manager.h"
#include "tsm_ui_manager.h"

#include <SDL.h>

#ifdef _WIN32
    #undef main
    #define SDL_MAIN_HANDLED
#endif

namespace
{
void LoadConfiguredAssets(const TSM::AppConfig& config)
{
    auto& audioManager = TSM::AudioManager::GetInstance();
    auto& playlistManager = TSM::PlaylistManager::GetInstance();
    auto& announcementManager = TSM::AnnouncementManager::GetInstance();

    audioManager.SetLoudnessTarget(config.loudnessTargetLufs);
    for (const auto& playlist : config.playlists)
    {
        playlistManager.CreatePlaylist(playlist.name);
        for (const auto& track : playlist.tracks)
        {
            if (audioManager.LoadSound(track.id, track.path, true))
            {
                playlistManager.AddToPlaylist(playlist.name, track.id);
            }
        }

        if (auto* loadedPlaylist = playlistManager.GetPlaylistByName(playlist.name))
        {
            loadedPlaylist->options = playlist.options;
        }
    }

    if (!config.wedding.entrance.empty())
        audioManager.LoadWeddingEntranceSound(config.wedding.entrance);
    if (!config.wedding.ceremony.empty())
        audioManager.LoadWeddingCeremonySound(config.wedding.ceremony);
    if (!config.wedding.exit.empty())
        audioManager.LoadWeddingExitSound(config.wedding.exit);
    if (!config.wedding.transitionSfx.id.empty() && !config.wedding.transitionSfx.path.empty())
    {
        audioManager.LoadSound(
            config.wedding.transitionSfx.id, config.wedding.transitionSfx.path, false);
    }

    for (const auto& announcement : config.announcements)
    {
        if (!audioManager.LoadAnnouncement(announcement.id, announcement.path)) continue;
        if (announcement.hour >= 0 && announcement.minute >= 0)
        {
            announcementManager.ScheduleAnnouncement(
                announcement.hour, announcement.minute, announcement.id);
        }
    }
}
}

int main()
{
    TSM::Logger::Init();
    if (!TSM::FModWrapper::GetInstance().Initialize())
    {
        spdlog::error("Failed to initialize FMOD.");
        return -1;
    }

    if (!TSM::UIManager::GetInstance().Init(1920, 1080))
    {
        spdlog::error("Failed to initialize GUI.");
        TSM::FModWrapper::GetInstance().Shutdown();
        return -1;
    }

    StartBluetoothServer();

    TSM::AppConfig config;
    std::string configError;
    if (TSM::LoadAppConfig("config/tsm_config.json", config, configError))
    {
        LoadConfiguredAssets(config);
    }
    else
    {
        spdlog::error("Application configuration was not loaded: {}", configError);
    }
    TSM::UIManager::GetInstance().UpdateWeddingFilePaths();

    bool isRunning = true;
    auto lastTime = std::chrono::high_resolution_clock::now();
    while (isRunning)
    {
        const auto currentTime = std::chrono::high_resolution_clock::now();
        const float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;

        ProcessPendingBluetoothCommands();
        TSM::AudioManager::GetInstance().Update(deltaTime);
        TSM::AnnouncementManager::GetInstance().Update(deltaTime);
        TSM::PlaylistManager::GetInstance().Update(deltaTime);
        TSM::UIManager::GetInstance().UpdateWeddingMode(deltaTime);

        TSM::UIManager::GetInstance().HandleEvents();
        TSM::UIManager::GetInstance().PreRender();
        TSM::UIManager::GetInstance().Render();
        TSM::UIManager::GetInstance().PostRender();

        if (!TSM::UIManager::GetInstance().IsRunning()) isRunning = false;
    }

    StopBluetoothServer();
    TSM::AudioManager::GetInstance().Shutdown();
    TSM::UIManager::GetInstance().Shutdown();
    TSM::FModWrapper::GetInstance().Shutdown();
    return 0;
}
