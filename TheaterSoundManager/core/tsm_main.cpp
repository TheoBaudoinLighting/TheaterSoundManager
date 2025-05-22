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

    const std::string preShowPlaylist = "playlist_PreShow";
    TSM::PlaylistManager::GetInstance().CreatePlaylist(preShowPlaylist);

    TSM::AudioManager::GetInstance().LoadSound("temps_amour", "assets/musics/PreShowMariage/01_Bon_Entendeur_Le_temps_de_l_amour.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("fio_maravilha", "assets/musics/PreShowMariage/Nicoletta_FioMaravilha.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("helwa_ya_baladi", "assets/musics/PreShowMariage/DalidaHelwaYaBaladi_LinaSleibiCover.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("laisse_moi_taimer", "assets/musics/PreShowMariage/LaisseMoiTaimer_MikeBrant.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("yeux_emilie", "assets/musics/PreShowMariage/JoeDassin_LesYeuxEmilie.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("bonnie_clyde", "assets/musics/PreShowMariage/SergeGainsbourg_BonnieClyde.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("perche_ti_amo", "assets/musics/PreShowMariage/SaraPercheTiAmo_NextGenRemix.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("emmenez_moi", "assets/musics/PreShowMariage/CharlesAznavour_EmmenezMoi.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("boheme", "assets/musics/PreShowMariage/CharlesAznavour_LaBohemeBoubouRemix.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("amour", "assets/musics/PreShowMariage/Mouloudji_Lamour.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("foule", "assets/musics/PreShowMariage/EdithPiaf_LaFoule.mp3", true);

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

    const std::string postShowPlaylist = "playlist_PostShow";
    TSM::PlaylistManager::GetInstance().CreatePlaylist(postShowPlaylist);

    TSM::AudioManager::GetInstance().LoadSound("bon_voyage", "assets/musics/PostShow/BonVoyage.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("buddha_bar", "assets/musics/PostShow/BuddhaBarIII.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("jazz_port_orleans", "assets/musics/PostShow/JazzPortOrleans.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("coffee_portofino", "assets/musics/PostShow/CoffeePortofino.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("country_bear", "assets/musics/PostShow/CountryBear.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("fantasy_spring", "assets/musics/PostShow/FantasySpring.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("forest_caffee", "assets/musics/PostShow/ForestCaffee.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("grand_avenue", "assets/musics/PostShow/GrandAvenue.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("hth", "assets/musics/PostShow/HTH.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("jazz_01", "assets/musics/PostShow/Jazz01.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("mmrr_exit_music", "assets/musics/PostShow/MMRR_ExitMusic.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("mmrr_lobby", "assets/musics/PostShow/MMRR_Lobby.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("bella_note", "assets/musics/PostShow/BellaNote.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("secret_love", "assets/musics/PostShow/SecretLove.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("agrabah_cafe_restaurant", "assets/musics/PostShow/AgrabahCafeRestaurant.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("tiana_palace", "assets/musics/PostShow/TianaPalace.mp3", true);
    TSM::AudioManager::GetInstance().LoadSound("town_center", "assets/musics/PostShow/TownCenter.mp3", true);

    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "bon_voyage");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "buddha_bar");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "jazz_port_orleans");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "coffee_portofino");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "country_bear");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "fantasy_spring");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "forest_caffee");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "grand_avenue");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "hth");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "jazz_01");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "mmrr_exit_music");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "mmrr_lobby");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "bella_note");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "secret_love");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "agrabah_cafe_restaurant");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "tiana_palace");
    TSM::PlaylistManager::GetInstance().AddToPlaylist(postShowPlaylist, "town_center");

    TSM::AudioManager::GetInstance().LoadWeddingEntranceSound("assets/wedding/Entree_RunrigHeartsOfOldenGlory.mp3");
    TSM::AudioManager::GetInstance().LoadWeddingCeremonySound("assets/wedding/Loop_PortOrleanAmbience.mp3");
    TSM::AudioManager::GetInstance().LoadWeddingExitSound("assets/musics/PreShowMariage/MariagedAmour_PauldeSennevilleJacobsPiano.mp3");

    TSM::AudioManager::GetInstance().LoadSound("sfx_shine", "assets/sfx/DisneyShine_SFX.mp3", false);
    
    // DAY 01
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_bienvenue_01", "assets/annonces/Both/bienvenue_01.mp3"); // 12h00

    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_15min_cl", "assets/annonces/Both/15min_cl.mp3"); // 12h15
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_10min_cl", "assets/annonces/Both/10min_cl.mp3"); // 12h20
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_5min_cl", "assets/annonces/Both/5min_cl.mp3"); // 12h25

    // 13h30 End of Ceremony

    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_15min_buffet", "assets/annonces/Both/15min_buffet.mp3"); // 14h15
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_10min_buffet", "assets/annonces/Both/10min_buffet.mp3"); // 14h20
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_5min_buffet", "assets/annonces/Both/5min_buffet.mp3"); // 14h25

    // 15h30 End of Buffet

    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_machine_photo", "assets/annonces/Both/machine_photo_02.mp3"); // 15h00
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_jeux_de_societer", "assets/annonces/Both/jeux_societer_01.mp3"); // 15h15
    TSM::AudioManager::GetInstance().LoadAnnouncement("announce_remerciements", "assets/annonces/Both/merci_01.mp3"); // 16h00

    // Programmation des annonces
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(12, 00, "announce_bienvenue_01");
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(12, 15, "announce_15min_cl");
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(12, 20, "announce_10min_cl");
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(12, 25, "announce_5min_cl");

    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(14, 15, "announce_15min_buffet");
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(14, 20, "announce_10min_buffet");
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(14, 25, "announce_5min_buffet");

    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(15, 00, "announce_machine_photo");
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(15, 15, "announce_jeux_de_societer");
    TSM::AnnouncementManager::GetInstance().ScheduleAnnouncement(16, 00, "announce_remerciements");
    


















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
