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

    if (!TSM::UIManager::GetInstance().Init(1920, 1080))
    {
        spdlog::error("Failed to initialize GUI.");
        TSM::FModWrapper::GetInstance().Shutdown();
        return -1;
    }

    StartBluetoothServer();

    // Création de la playlist par défaut
    const std::string defaultPlaylist = "playlist_test";
    TSM::PlaylistManager::GetInstance().CreatePlaylist(defaultPlaylist);

    // Chargement des musiques pour les phases du mode mariage
    TSM::AudioManager::GetInstance().LoadSound("hi_diddle_dee", "assets/musics/Entrance(Fantasia Gardens) - Hi diddle dee dee (Pinocchio).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("be_our_guest", "assets/musics/Entrance(Fantasia Gardens) - Be our guest (Beauty and the beast).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("rise_and_shine", "assets/musics/Entrance(Fantasia Gardens) - Rise and shine (Monsters university).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("supercalifragilisticexpialidocious", "assets/musics/Entrance(Fantasia Gardens) - Supercalifragilisticexpialidocious (Mary Poppins).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("walk_to_work", "assets/musics/Entrance(Fantasia Gardens) - Walk to work (Monsters inc. ).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("pinocchio_village_haus", "assets/musics/Pinocchio Village Haus Magic Kingdom Walt Disney World full Area music (1991-present).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("indiana_jones", "assets/musics/The Music Of Indiana Jones And The Temple Of Peril・Disneyland Paris (BGMComplete Loop).mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("mickey_minnie_runaway_railway", "assets/musics/Mickey & Minnie's Runaway Railway - Full Exit Music.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("royal_street", "assets/musics/RoyalStreet - Area Background Music  at Tokyo Disneyland.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("whistle_while_you_work", "assets/musics/Entrance(Fantasia Gardens) - Whistle while you work (Snow white).mp3", true);

    // Ajout des musiques à la playlist par défaut
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "hi_diddle_dee");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "be_our_guest");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "rise_and_shine");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "supercalifragilisticexpialidocious");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "walk_to_work");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "pinocchio_village_haus");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "indiana_jones");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "mickey_minnie_runaway_railway");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "royal_street");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(defaultPlaylist, "whistle_while_you_work");

    // Chargement des musiques pour les phases du mode mariage
    TSM::AudioManager::GetInstance().LoadWeddingEntranceSound("assets/wedding/Entree_RunrigHeartsOfOldenGlory.mp3");
    TSM::AudioManager::GetInstance().LoadWeddingCeremonySound("assets/wedding/Loop_PortOrleanAmbience.mp3");
    TSM::AudioManager::GetInstance().LoadWeddingExitSound("assets/wedding/Camille Saint-Saens - The Swan.mp3");

    // Chargement des effets sonores
    TSM::AudioManager::GetInstance().LoadSound("sfx_shine", "assets/sfx/DisneyShine_SFX.mp3", false);
    
    // Chargement des annonces
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_5min_buffet", "assets/annonces/French/5min_buffet.mp3");
    
    // Configuration d'un déclenchement temporisé d'annonce
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(19, 45, "announce_5min_buffet");
    
    // Mettre à jour les chemins de fichiers des musiques de mariage
    TSM::UIManager::GetInstance().UpdateWeddingFilePaths();

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
        TSM::UIManager::GetInstance().UpdateWeddingMode(dt);

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
