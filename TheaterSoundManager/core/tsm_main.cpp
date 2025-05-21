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
    const std::string preShowPlaylist = "playlist_PreShow";
    TSM::PlaylistManager::GetInstance().CreatePlaylist(preShowPlaylist);

    // Chargement des musiques pour le PreShow Mariage
    TSM::AudioManager::GetInstance().LoadSound("temps_amour", "assets/musics/PreShowMariage/01_Bon_Entendeur_Le_temps_de_l_amour.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("fio_maravilha", "assets/musics/PreShowMariage/Nicoletta_FioMaravilha.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("helwa_ya_baladi", "assets/musics/PreShowMariage/DalidaHelwaYaBaladi_LinaSleibiCover.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("laisse_moi_taimer", "assets/musics/PreShowMariage/LaisseMoiTaimer_MikeBrant.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("yeux_emilie", "assets/musics/PreShowMariage/JoeDassin_LesYeuxEmilie.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("bonnie_clyde", "assets/musics/PreShowMariage/SergeGainsbourg_BonnieClyde.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("perche_ti_amo", "assets/musics/PreShowMariage/SaraPercheTiAmo_NextGenRemix.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("emmenez_moi", "assets/musics/PreShowMariage/CharlesAznavour_EmmenezMoi.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("boheme", "assets/musics/PreShowMariage/CharlesAznavour_ LaBohemeBoubouRemix.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("amour", "assets/musics/PreShowMariage/Mouloudji_Lamour.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("foule", "assets/musics/PreShowMariage/EdithPiaf_LaFoule.mp3", true);

    // Ajout des musiques à la playlist par défaut
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "temps_amour");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "fio_maravilha");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "helwa_ya_baladi");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "laisse_moi_taimer");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "yeux_emilie");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "bonnie_clyde");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "perche_ti_amo");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "emmenez_moi");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "boheme");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "amour");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(preShowPlaylist, "foule");

    // Chargement des musiques pour les phases du mode mariage
    TSM::AudioManager::GetInstance().LoadWeddingEntranceSound("assets/wedding/Entree_RunrigHeartsOfOldenGlory.mp3");
    TSM::AudioManager::GetInstance().LoadWeddingCeremonySound("assets/wedding/Loop_PortOrleanAmbience.mp3");
    TSM::AudioManager::GetInstance().LoadWeddingExitSound("assets/musics/PreShowMariage/MariagedAmour_PauldeSennevilleJacobsPiano.mp3");

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
