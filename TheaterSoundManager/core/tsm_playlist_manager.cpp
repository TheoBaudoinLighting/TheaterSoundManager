// tsm_playlist_manager.cpp

#include "tsm_playlist_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"
#include "tsm_ui_manager.h"

#include <algorithm>
#include <random>
#include <ctime>
#include <cmath>

#include <spdlog/spdlog.h>

namespace TSM
{
    
void PlaylistManager::CreatePlaylist(const std::string& playlistName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it == m_playlists.end())
    {
        Playlist newPlaylist;
        newPlaylist.name = playlistName;
        m_playlists.push_back(newPlaylist);
        spdlog::info("Playlist '{}' created.", playlistName);
    }
    else
    {
        spdlog::error("Playlist '{}' already exists.", playlistName);
    }
}

void PlaylistManager::AddToPlaylist(const std::string& playlistName, const std::string& soundName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        it->tracks.push_back(soundName);
        spdlog::info("Added '{}' to playlist '{}'.", soundName, playlistName);
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
    }
}

void PlaylistManager::RemoveFromPlaylist(const std::string& playlistName, const std::string& soundName)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it != m_playlists.end())
    {
        auto& tracks = it->tracks;
        tracks.erase(std::remove(tracks.begin(), tracks.end(), soundName), tracks.end());
        spdlog::info("Removed '{}' from playlist '{}'.", soundName, playlistName);
    }
    else
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
    }
}

void PlaylistManager::Play(const std::string& playlistName, const PlaylistOptions& options)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p) { return p.name == playlistName; });

    if (it == m_playlists.end())
    {
        spdlog::error("Playlist '{}' not found.", playlistName);
        return;
    }

    Playlist& plist = *it;
    if (plist.tracks.empty())
    {
        spdlog::error("Playlist '{}' is empty.", playlistName);
        return;
    }

    Stop(m_activePlaylistName);
    
    m_activePlaylistName = playlistName;
    plist.options = options;
    plist.isPlaying = true;

    if (options.randomOrder)
    {
        PrepareRandomOrder(plist);
        plist.randomIndexPos = 0;
        plist.currentIndex = plist.randomIndices[0];
    }
    else
    {
        plist.currentIndex = 0;
    }

    plist.currentChannel = nullptr;
    plist.nextChannel = nullptr;
    plist.isCrossfading = false;
    plist.crossfadeTimer = 0.0f;

    plist.segmentModeActive = options.randomSegment;
    plist.segmentMaxDuration = options.segmentDuration;
    plist.segmentTimer = 0.0f;
    plist.chosenStartTime = 0.0f;

    StartTrackAtIndex(plist, plist.currentIndex);

    spdlog::info("Playlist '{}' started.", playlistName);
}

void PlaylistManager::Stop(const std::string& playlistName)
{
    if (playlistName.empty())
    {
        for (auto& plist : m_playlists)
        {
            if (plist.isPlaying)
            {
                if (plist.currentChannel)
                {
                    bool isPlaying = false;
                    plist.currentChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        std::string currentTrack = plist.tracks[plist.currentIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(currentTrack);
                    }
                }
                
                if (plist.nextChannel)
                {
                    bool isPlaying = false;
                    plist.nextChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        int nextIndex = (plist.currentIndex + 1) % plist.tracks.size();
                        std::string nextTrack = plist.tracks[nextIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(nextTrack);
                    }
                }
                
                plist.isPlaying = false;
                plist.isCrossfading = false;
                plist.currentChannel = nullptr;
                plist.nextChannel = nullptr;
            }
        }
        
        spdlog::info("Toutes les playlists ont été arrêtées avec fade-out");
    }
    else
    {
        for (auto& plist : m_playlists)
        {
            if (plist.name == playlistName && plist.isPlaying)
            {
                if (plist.currentChannel)
                {
                    bool isPlaying = false;
                    plist.currentChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        std::string currentTrack = plist.tracks[plist.currentIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(currentTrack);
                    }
                }
                
                if (plist.nextChannel)
                {
                    bool isPlaying = false;
                    plist.nextChannel->isPlaying(&isPlaying);
                    if (isPlaying)
                    {
                        int nextIndex = (plist.currentIndex + 1) % plist.tracks.size();
                        std::string nextTrack = plist.tracks[nextIndex];
                        AudioManager::GetInstance().StopSoundWithFadeOut(nextTrack);
                    }
                }
                
                plist.isPlaying = false;
                plist.isCrossfading = false;
                plist.currentChannel = nullptr;
                plist.nextChannel = nullptr;
                
                spdlog::info("Playlist '{}' arrêtée avec fade-out", playlistName);
                break;
            }
        }
    }
}

void PlaylistManager::Update(float deltaTime)
{
    for (auto& plist : m_playlists)
    {
        if (!plist.isPlaying) continue;

        float baseMusicVol = UIManager::GetInstance().GetMasterVolume() 
                           * UIManager::GetInstance().GetMusicVolume()
                           * UIManager::GetInstance().GetDuckFactor();

        if (plist.isCrossfading)
        {
            plist.crossfadeTimer += deltaTime;
            float t = plist.crossfadeTimer / plist.crossfadeDuration;
            if (t > 1.0f) t = 1.0f;

            bool currentChannelValid = false;
            bool nextChannelValid    = false;

            if (plist.currentChannel)
            {
                bool isPlaying = false;
                FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
                currentChannelValid = (result == FMOD_OK && isPlaying);
                
                if (currentChannelValid) {
                    float volOld = (1.0f - t) * baseMusicVol;
                    plist.currentChannel->setVolume(volOld);
                }
            }

            if (plist.nextChannel)
            {
                bool isPlaying = false;
                FMOD_RESULT result = plist.nextChannel->isPlaying(&isPlaying);
                nextChannelValid = (result == FMOD_OK && isPlaying);
                
                if (nextChannelValid) {
                    
                    float volNext = t * baseMusicVol;
                    plist.nextChannel->setVolume(volNext);
                }
            }

            if (t >= 1.0f || (!currentChannelValid && !nextChannelValid))
            {
                FinishCrossfade(plist);
            }
        }
        else if (plist.currentChannel)
        {
            bool isPlaying = false;
            FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
            
            if (result != FMOD_OK)
            {
                spdlog::error("Channel state check failed, restarting track");
                StartTrackAtIndex(plist, plist.currentIndex);
                continue;
            }

            if (!isPlaying && !plist.segmentModeActive)
            {
                if (plist.options.loopPlaylist && plist.tracks.size() == 1)
                {
                    StartTrackAtIndex(plist, plist.currentIndex);
                }
                else if (plist.options.loopPlaylist || plist.tracks.size() > 1)
                {
                    StartNextTrack(plist);
                }
                else
                {
                    plist.isPlaying = false;
                    spdlog::info("Playlist '{}' finished playing (no loop option).", plist.name);
                }
                continue;
            }

            if (plist.segmentModeActive)
            {
                FMOD::Sound* sound = nullptr;
                result = plist.currentChannel->getCurrentSound(&sound);
                if (result == FMOD_OK && sound)
                {
                    unsigned int lengthMs = 0;
                    if (sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS) == FMOD_OK)
                    {
                        float lengthSec = lengthMs / 1000.0f;
                        float effectiveSegmentDuration = std::min(plist.segmentMaxDuration, lengthSec);
                        
                        plist.segmentTimer += deltaTime;
                        if (plist.segmentTimer >= effectiveSegmentDuration)
                        {
                            if (plist.tracks.size() == 1)
                            {
                                StartTrackAtIndex(plist, plist.currentIndex);
                            }
                            else
                            {
                                StartNextTrack(plist);
                            }
                            continue;
                        }
                    }
                }
            }
        }
        else
        {
            spdlog::warn("Invalid channel state detected, attempting to recover");
            StartTrackAtIndex(plist, plist.currentIndex);
        }
    }
}

std::string PlaylistManager::GetCurrentTrackName() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->isPlaying) return "";
    if (activePlaylist->currentIndex < 0 || 
        activePlaylist->currentIndex >= (int)activePlaylist->tracks.size()) 
        return "";
    return activePlaylist->tracks[activePlaylist->currentIndex];
}

float PlaylistManager::GetTrackProgress() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->currentChannel) 
        return 0.0f;

    unsigned int positionMs = 0;
    unsigned int lengthMs   = 0;
    FMOD::Sound* sound      = nullptr;

    FMOD_RESULT result = activePlaylist->currentChannel->getPosition(&positionMs, FMOD_TIMEUNIT_MS);
    if (result != FMOD_OK) return 0.0f;

    result = activePlaylist->currentChannel->getCurrentSound(&sound);
    if (result != FMOD_OK || !sound) return 0.0f;

    result = sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    if (result != FMOD_OK || lengthMs == 0) return 0.0f;

    return static_cast<float>(positionMs) / static_cast<float>(lengthMs);
}

float PlaylistManager::GetSegmentProgress() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->segmentModeActive) return 0.0f;
    if (activePlaylist->segmentMaxDuration <= 0.0f) return 0.0f;
    return activePlaylist->segmentTimer / activePlaylist->segmentMaxDuration;
}

void PlaylistManager::SetCrossfadeDuration(float duration)
{
    if (m_currentPlaylist) {
        m_currentPlaylist->crossfadeDuration = duration;
    }
}

FMOD::Channel* PlaylistManager::GetCurrentChannel() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist ? activePlaylist->currentChannel : nullptr;
}

bool PlaylistManager::IsInCrossfade() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist ? activePlaylist->isCrossfading : false;
}

float PlaylistManager::GetCrossfadeProgress() const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || !activePlaylist->isCrossfading) return 0.0f;
    return activePlaylist->crossfadeTimer / activePlaylist->crossfadeDuration;
}

FMOD::Channel* PlaylistManager::GetNextChannel() const
{
    auto* activePlaylist = GetActivePlaylist();
    return activePlaylist ? activePlaylist->nextChannel : nullptr;
}

const PlaylistManager::Playlist* PlaylistManager::GetActivePlaylist() const
{
    if (m_activePlaylistName.empty()) return nullptr;
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [this](const Playlist& p) { return p.name == m_activePlaylistName; });
    return (it != m_playlists.end()) ? &(*it) : nullptr;
}

PlaylistManager::Playlist* PlaylistManager::GetActivePlaylist()
{
    if (m_activePlaylistName.empty()) return nullptr;
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [this](Playlist& p) { return p.name == m_activePlaylistName; });
    return (it != m_playlists.end()) ? &(*it) : nullptr;
}

void PlaylistManager::StartNextTrack(Playlist& plist)
{
    int nextIndex = 0;
    if (plist.options.randomOrder)
    {
        plist.randomIndexPos++;
        if (plist.randomIndexPos >= (int)plist.randomIndices.size())
        {
            if (plist.options.loopPlaylist) {
                plist.randomIndexPos = 0;
            } else {
                plist.isPlaying = false;
                return;
            }
        }
        nextIndex = plist.randomIndices[plist.randomIndexPos];
    }
    else
    {
        nextIndex = plist.currentIndex + 1;
        if (nextIndex >= (int)plist.tracks.size())
        {
            if (plist.options.loopPlaylist) {
                nextIndex = 0;
            } else {
                plist.isPlaying = false;
                return;
            }
        }
    }

    plist.isCrossfading   = true;
    plist.crossfadeTimer  = 0.0f;

    plist.oldChannelVolume = 1.0f;
    if (plist.currentChannel)
    {
        float vol = 1.0f;
        plist.currentChannel->getVolume(&vol);
        plist.oldChannelVolume = vol;
    }

    float userVolume = UIManager::GetInstance().GetMasterVolume() 
                     * UIManager::GetInstance().GetMusicVolume();
    plist.nextTargetVolume = userVolume;

    std::string nextTrack = plist.tracks[nextIndex];
    FMOD::Channel* ch = AudioManager::GetInstance().PlaySound(nextTrack, false, 0.0f);
    plist.nextChannel = ch;

    plist.currentIndex = nextIndex;

    plist.segmentTimer = 0.0f;
    if (plist.segmentModeActive && ch)
    {
        FMOD::Sound* sound = nullptr;
        ch->getCurrentSound(&sound);
        if (sound)
        {
            unsigned int lengthMs = 0;
            sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
            float lengthSec = lengthMs / 1000.0f;

            float maxStart = (lengthSec > plist.segmentMaxDuration)
                             ? (lengthSec - plist.segmentMaxDuration)
                             : 0.0f;
            std::uniform_real_distribution<float> dist(0.0f, maxStart);
            plist.chosenStartTime = dist(m_rng);

            ch->setPosition((unsigned int)(plist.chosenStartTime * 1000.0f), FMOD_TIMEUNIT_MS);
        }
    }
}

void PlaylistManager::StartTrackAtIndex(Playlist& plist, int index)
{
    if (index < 0 || index >= (int)plist.tracks.size()) return;

    std::string track = plist.tracks[index];

    float userVolume = UIManager::GetInstance().GetMasterVolume() 
                     * UIManager::GetInstance().GetMusicVolume();

    bool doLoop = (!plist.options.randomSegment && plist.options.loopPlaylist && plist.tracks.size() == 1);
    
    FMOD::Channel* ch = AudioManager::GetInstance().PlaySound(track, doLoop, userVolume);
    if (!ch) {
        spdlog::error("Failed to start track at index {}", index);
        return;
    }

    if (plist.currentChannel) {
        bool isPlaying = false;
        FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
        if (result == FMOD_OK && isPlaying) {
            plist.currentChannel->stop();
        }
    }

    plist.currentChannel = ch;

    bool isPlaying = false;
    FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
    if (result != FMOD_OK) {
        spdlog::error("Channel validation failed after start");
        return;
    }

    plist.segmentTimer = 0.0f;
    
    if (plist.options.randomSegment && ch)
    {
        plist.segmentModeActive = true;
        
        FMOD::Sound* sound = nullptr;
        ch->getCurrentSound(&sound);
        if (sound)
        {
            unsigned int lengthMs = 0;
            sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
            float lengthSec = lengthMs / 1000.0f;

            float maxStart = (lengthSec > plist.options.segmentDuration)
                             ? (lengthSec - plist.options.segmentDuration)
                             : 0.0f;
            
            std::uniform_real_distribution<float> dist(0.0f, maxStart);
            plist.chosenStartTime = dist(m_rng);
            
            ch->setPosition((unsigned int)(plist.chosenStartTime * 1000.0f), FMOD_TIMEUNIT_MS);
            
            plist.segmentMaxDuration = plist.options.segmentDuration;
        }
    }
    else
    {
        plist.segmentModeActive = false;
    }
}

void PlaylistManager::FinishCrossfade(Playlist& plist)
{
    if (plist.currentChannel)
    {
        bool isPlaying = false;
        FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
        if (result == FMOD_OK && isPlaying) {
            plist.currentChannel->stop();
        }
        plist.currentChannel = nullptr;
    }

    plist.currentChannel = plist.nextChannel;
    plist.nextChannel    = nullptr;
    plist.isCrossfading  = false;
    plist.crossfadeTimer = 0.0f;

    if (plist.currentChannel)
    {
        bool isPlaying = false;
        FMOD_RESULT result = plist.currentChannel->isPlaying(&isPlaying);
        if (result != FMOD_OK || !isPlaying)
        {
            spdlog::warn("New current channel invalid after crossfade, restarting track");
            StartTrackAtIndex(plist, plist.currentIndex);
        }
    }
}

void PlaylistManager::PrepareRandomOrder(Playlist& plist)
{
    int size = (int)plist.tracks.size();
    plist.randomIndices.resize(size);
    for (int i = 0; i < size; i++)
        plist.randomIndices[i] = i;

    std::shuffle(plist.randomIndices.begin(), plist.randomIndices.end(), m_rng);
}

const PlaylistManager::Playlist* PlaylistManager::GetPlaylistByName(const std::string& name) const
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&name](const Playlist& p){ return p.name == name; });
    if (it != m_playlists.end()) {
        return &(*it);
    }
    return nullptr;
}

void PlaylistManager::MoveTrackUp(const std::string& playlistName, int index)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;
    auto& plist = *it;
    if (index <= 0 || index >= (int)plist.tracks.size()) return;

    std::swap(plist.tracks[index], plist.tracks[index-1]);
}

void PlaylistManager::MoveTrackDown(const std::string& playlistName, int index)
{
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;
    auto& plist = *it;
    if (index < 0 || index >= (int)plist.tracks.size() - 1) return;

    std::swap(plist.tracks[index], plist.tracks[index+1]);
}

void PlaylistManager::PlayFromIndex(const std::string& playlistName, int index)
{
    Stop(playlistName);

    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&playlistName](const Playlist& p){ return p.name == playlistName; });
    if (it == m_playlists.end()) return;

    Playlist& plist = *it;
    if (index < 0 || index >= (int)plist.tracks.size()) return;

    if (!plist.isPlaying) {
        PlaylistOptions opts;
        plist.options = opts;
    }
    
    plist.isPlaying = true;
    plist.currentIndex = index;
    plist.currentChannel = nullptr;
    plist.nextChannel = nullptr;
    plist.isCrossfading = false;
    plist.crossfadeTimer = 0.0f;

    plist.segmentModeActive = plist.options.randomSegment;
    plist.segmentTimer = 0.0f;

    StartTrackAtIndex(plist, index);
}

std::string PlaylistManager::GetTrackName(int index) const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || index < 0 || index >= (int)activePlaylist->tracks.size()) return "";
    return activePlaylist->tracks[index];
}

std::string PlaylistManager::GetTrackDuration(int index) const
{
    auto* activePlaylist = GetActivePlaylist();
    if (!activePlaylist || index < 0 || index >= (int)activePlaylist->tracks.size()) return "";
    return activePlaylist->tracks[index];
}

} // namespace TSM
