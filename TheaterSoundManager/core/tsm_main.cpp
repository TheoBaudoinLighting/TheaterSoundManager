// tsm_main.cpp

#include <iostream>
#include <thread>
#include <chrono>
#include "tsm_fmod_wrapper.h"
#include "tsm_audio_manager.h"
#include "tsm_announcement_manager.h"
#include "tsm_playlist_manager.h"
#include "tsm_ui_manager.h"
#include "tsm_logger.h"

// Bluetooth
#include "tsm_bluetooth_server.h"

#include <SDL.h>
#include <SDL_opengl.h>

#ifdef _WIN32
    #undef main
    #define SDL_MAIN_HANDLED
#endif

int main()
{
    TSM::Logger::Init();
    if (!TSM::FModWrapper::GetInstance().Initialize())
    {
        spdlog::error("Failed to initialize FMOD.");
        return -1;
    }

    if (!TSM::UIManager::GetInstance().Init(1280, 900))
    {
        spdlog::error("Failed to initialize GUI.");
        TSM::FModWrapper::GetInstance().Shutdown();
        return -1;
    }

    StartBluetoothServer();

    // Exemple d'utilisation du gestionnaire audio et playlist (les musiques sont commentées)
    /*TSM::AudioManager::GetInstance().LoadSound("musique1", "assets/musics/Entrance(Fantasia Gardens) - Rise and shine (Monsters university).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("musique2", "assets/musics/Entrance(Fantasia Gardens) - Hi diddle dee dee (Pinocchio).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("musique3", "assets/musics/Entrance(Fantasia Gardens) - Be our guest (Beauty and the beast).mp3", true);
    
    TSM::PlaylistManager::GetInstance().CreatePlaylist("playlist_test");
    TSM::PlaylistManager::GetInstance().AddToPlaylist("playlist_test", "musique1");
    TSM::PlaylistManager::GetInstance().AddToPlaylist("playlist_test", "musique2");
    TSM::PlaylistManager::GetInstance().AddToPlaylist("playlist_test", "musique3");*/

    TSM::AudioManager::GetInstance().LoadSound("sfx_shine",  "assets/sfx/DisneyShine_SFX.mp3", false);
    //TSM::AnnouncementManager::GetInstance().LoadAnnouncement("announce1", "assets/announce/5min_buffet.wav");

    TSM::AnnouncementManager::GetInstance().LoadAnnouncement("announce1", "assets/announce/Temp/Both/5min_buffet.mp3");

    bool isRunning = true;
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (isRunning)
    {
        auto currentTime = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;

        TSM::AudioManager::GetInstance().Update(dt);
        TSM::AnnouncementManager::GetInstance().Update(dt);
        TSM::PlaylistManager::GetInstance().Update(dt);

        TSM::UIManager::GetInstance().HandleEvents();

        TSM::UIManager::GetInstance().PreRender();
        TSM::UIManager::GetInstance().Render();
        TSM::UIManager::GetInstance().PostRender();

        if (!TSM::UIManager::GetInstance().IsRunning()) {
            isRunning = false;
        }
    }

    TSM::AudioManager::GetInstance().StopAllSounds();
    TSM::UIManager::GetInstance().Shutdown();
    TSM::FModWrapper::GetInstance().Shutdown();

    return 0;
}
