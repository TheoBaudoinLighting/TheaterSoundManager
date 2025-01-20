// tsm_announcement.cpp

#include "tsm_announcement.h"
#include <filesystem>
#include <chrono>
#include <thread>

#include "tsm_lock_manager.h"

namespace fs = std::filesystem;

AnnouncementManager::AnnouncementManager(FMOD::System* system)
    : announcementChannel(nullptr)
    , fmodSystem(system)
    , musicDuckFactor(0.1f)
    , announcementVolumeMultiplier(3.0f)
    , isAnnouncementPlaying(false)
    , currentMusicOriginalVolume(1.0f)
{
}

AnnouncementManager::~AnnouncementManager() {
    TSM_LOCK_NAMED(PLAYBACK, "gestion_annonces", lock);
    for (auto& announcement : announcements) {
        if (announcement.sound) {
            announcement.sound->release();
            announcement.sound = nullptr;
        }
    }
    announcements.clear();
}

bool AnnouncementManager::isChannelPlaying(FMOD::Channel* channel) const {
    if (!channel) return false;
    bool isPlaying = false;
    channel->isPlaying(&isPlaying);
    return isPlaying;
}

void AnnouncementManager::doVolumeFade(FMOD::Channel* channel, float startVolume, float endVolume, int durationMs) {
    if (!channel) return;

    std::thread([this, channel, startVolume, endVolume, durationMs]() {
        bool isPlaying = false;
        channel->isPlaying(&isPlaying);
        if (!isPlaying) return;

        const int steps = 50;
        int stepDuration = durationMs / steps;
        float volumeDelta = (endVolume - startVolume) / steps;

        for (int i = 0; i < steps; i++) {
            channel->isPlaying(&isPlaying);
            if (!isPlaying) return;
            
            float currentVolume = startVolume + (volumeDelta * i);
            channel->setVolume(currentVolume);
            std::this_thread::sleep_for(std::chrono::milliseconds(stepDuration));
        }

        channel->isPlaying(&isPlaying);
        if (isPlaying) {
            channel->setVolume(endVolume);
        }
    }).detach();
}

bool AnnouncementManager::waitForChannelToFinish(FMOD::Channel* channel) {
    while (isChannelPlaying(channel)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

void AnnouncementManager::loadAnnouncement(const std::string& filepath) {
    if (!fmodSystem) return;
    TSM_LOCK_NAMED(PLAYBACK, "chargement_annonce", lock);

    std::string filename = fs::path(filepath).filename().string();
    
    spdlog::info("Attempting to load announcement:");
    spdlog::info("Full path: {}", filepath);
    spdlog::info("Filename: {}", filename);
    
    if (!fs::exists(filepath)) {
        spdlog::error("Error: File does not exist: {}", filepath);
        return;
    }

    FMOD::Sound* sound = nullptr;
    FMOD_RESULT result = fmodSystem->createSound(filepath.c_str(), FMOD_DEFAULT, nullptr, &sound);
    
    if (result == FMOD_OK) {
        TSM_LOCK_NAMED(PLAYBACK, "ajout_annonce", lock);
        Announcement announcement = {
            filepath,
            filename,
            sound,
            12,      
            0,        
            true,     
            1.0f    
        };
        announcements.push_back(announcement);
        spdlog::info("Announcement loaded successfully!");
    } else {
        spdlog::error("FMOD::createSound error for announcement: {}", std::to_string(static_cast<int>(result)));
        if (sound) {
            sound->release();
        }
    }
}

void AnnouncementManager::playAnnouncement(FMOD::Sound* annSound, float announcementVol, 
                                         FMOD::Channel* currentMusicChannel, float currentMusicVolume) 
{
    if (!fmodSystem || !annSound) return;

    {
        TSM_LOCK(PLAYBACK, "lecture_annonce");
        if (isAnnouncementPlaying) return;
        isAnnouncementPlaying = true;
    }

    if (currentMusicChannel) {
        bool musicPlaying = false;
        if (currentMusicChannel->isPlaying(&musicPlaying) == FMOD_OK && musicPlaying) {
            currentMusicOriginalVolume = currentMusicVolume;
            currentMusicChannel->setVolume(currentMusicOriginalVolume * musicDuckFactor);
        }
    }

    FMOD::Channel* annChannel = nullptr;
    FMOD_RESULT result = fmodSystem->playSound(annSound, nullptr, true, &annChannel);
    if (result == FMOD_OK && annChannel) {
        float finalVol = announcementVol * announcementVolumeMultiplier;
        annChannel->setVolume(finalVol);
        annChannel->setPaused(false);

        FMOD::Channel* annChannelCopy = annChannel;
        FMOD::Channel* musicChannelCopy = currentMusicChannel;
        
        std::thread([this, annChannelCopy, musicChannelCopy]() {
            try {
                bool playing = true;
                while (playing) {
                    if (!annChannelCopy || annChannelCopy->isPlaying(&playing) != FMOD_OK) {
                        playing = false;
                    }
                    if (playing) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }

                if (musicChannelCopy) {
                    bool stillPlaying = false;
                    if (musicChannelCopy->isPlaying(&stillPlaying) == FMOD_OK && stillPlaying) {
                        doVolumeFade(musicChannelCopy, 
                                   currentMusicOriginalVolume * musicDuckFactor,
                                   currentMusicOriginalVolume, 
                                   FADE_DURATION_MS);
                    }
                }

                {
                    TSM_LOCK(PLAYBACK, "fin_lecture_annonce");
                    isAnnouncementPlaying = false;
                }
            }
            catch (const std::exception& e) {
                spdlog::error("Exception dans le thread d'annonce: {}", e.what());
                TSM_LOCK(PLAYBACK, "erreur_lecture_annonce");
                isAnnouncementPlaying = false;
            }
        }).detach();
    }
    else {
        TSM_LOCK(PLAYBACK, "echec_lecture_annonce");
        isAnnouncementPlaying = false;
        spdlog::error("Impossible de jouer l'annonce: {}", static_cast<int>(result));
    }
}

void AnnouncementManager::checkAndPlayAnnouncements(PlaybackManager& playbackMgr) {
    auto now = std::chrono::system_clock::now();
    std::time_t nowT = std::chrono::system_clock::to_time_t(now);
    std::tm localTime;
#ifdef _WIN32
    localtime_s(&localTime, &nowT);
#else
    localtime_r(&nowT, &localTime);
#endif

    TSM_LOCK(PLAYBACK, "verification_annonces");

    FMOD::Channel* musicChannel = playbackMgr.getCurrentChannel();
    if (isAnnouncementPlaying) return;

    float currentMusicVol = 1.0f;
    if (musicChannel) {
        musicChannel->getVolume(&currentMusicVol);
    }

    for (auto& ann : announcements) {
        if (!ann.isEnabled) continue;
        if (localTime.tm_hour == ann.hour && localTime.tm_min == ann.minute) {
            playAnnouncement(ann.sound, ann.volume, musicChannel, currentMusicVol); 
            break;
        }
    }
}


void AnnouncementManager::setMusicDuckFactor(float factor) {
    musicDuckFactor = factor;
}

float AnnouncementManager::getMusicDuckFactor() const {
    return musicDuckFactor;
}

void AnnouncementManager::setAnnouncementVolumeMultiplier(float multiplier) {
    announcementVolumeMultiplier = multiplier;
}

float AnnouncementManager::getAnnouncementVolumeMultiplier() const {
    return announcementVolumeMultiplier;
}

bool AnnouncementManager::isPlaying() const {
    return isAnnouncementPlaying;
}

size_t AnnouncementManager::getAnnouncementCount() const {
    TSM_LOCK(PLAYBACK, "comptage_annonces");
    return announcements.size();
}

AnnouncementManager::Announcement* AnnouncementManager::getAnnouncement(size_t index) {
    TSM_LOCK(PLAYBACK, "acces_annonce");
    if (index >= announcements.size()) return nullptr;
    return &announcements[index];
}

const AnnouncementManager::Announcement* AnnouncementManager::getAnnouncement(size_t index) const {
    TSM_LOCK(PLAYBACK, "acces_annonce_const");
    if (index >= announcements.size()) return nullptr;
    return &announcements[index];
}

void AnnouncementManager::removeAnnouncement(size_t index) {
    TSM_LOCK(PLAYBACK, "suppression_annonce");
    if (index >= announcements.size()) return;
    if (announcements[index].sound) {
        announcements[index].sound->release();
    }
    announcements.erase(announcements.begin() + index);
}

void AnnouncementManager::testAnnouncement(size_t index, PlaybackManager& playbackMgr) {
    TSM_LOCK(PLAYBACK, "test_annonce");
    if (index >= announcements.size()) return;

    if (announcements[index].sound) {
        FMOD::Channel* musicChannel = playbackMgr.getCurrentChannel();
        float currentMusicVolume = 0.0f;
        if (musicChannel) {
            musicChannel->getVolume(&currentMusicVolume);
        }

        playAnnouncement(
            announcements[index].sound,
            announcements[index].volume,
            musicChannel,
            currentMusicVolume
        );
    }
}

void AnnouncementManager::forEachAnnouncement(std::function<void(Announcement&)> func) {
    TSM_LOCK(PLAYBACK, "iteration_annonces");
    for (auto& announcement : announcements) {
        func(announcement);
    }
}