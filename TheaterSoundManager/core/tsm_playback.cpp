// tsm_playback.cpp

#include "tsm_playback.h"
#include "tsm_logger.h"
#include <algorithm> 

PlaybackManager::PlaybackManager(FMOD::System* system)
    : fmodSystem(system)
    , currentTrackIndex(-1)
    , volume(1.0f)
    , randomOrder(false)
    , randomSegment(false)
    , isFadingOut(false)
    , isFadingIn(false)
    , fadeStartTime(0)
    , segmentStartTime(0)
    , fadeDuration(5000)
    , segmentDuration(120000)
    , lastTrackIndex(0)
    , lastSegmentOffset(0)
{
    if (!system) 
    {
        throw std::runtime_error("FMOD system is null");
    }

    channelManager.setSystem(system);
    nextChannelManager.setSystem(system);
    rng.seed(std::random_device()());
}

PlaybackManager::~PlaybackManager()
{
    if (!fmodSystem) return;
    stopPlayback();

    {
        TSM_LOCK(PLAYBACK, "PlaybackResource");
        for (auto& track : musicTracks) {
            if (track.sound) {
                track.sound->release();
                track.sound = nullptr;
            }
        }
        musicTracks.clear();
    }
}

bool PlaybackManager::isActuallyPlaying() const
{
    return channelManager.isPlaying() || nextChannelManager.isPlaying();
}

bool PlaybackManager::addTrack(const std::string& filepath, 
                               const std::string& filename, 
                               FMOD::Sound* sound,
                               unsigned int lengthMs)
{
    if (!sound) return false;
    
    try {
        TSM_LOCK(PLAYBACK, "PlaybackResource");
        
        MusicTrack mtrack;
        mtrack.filepath = filepath;
        mtrack.filename = filename;
        mtrack.sound = sound;
        mtrack.lengthMs = lengthMs;

        musicTracks.push_back(mtrack);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Lock acquisition failed in addTrack: {}", e.what());
        return false;
    }
}

bool PlaybackManager::removeTrack(size_t index)
{
    TSM_LOCK(PLAYBACK, "PlaybackResource");
    if (index >= musicTracks.size()) return false;

    if ((int)index == currentTrackIndex) {
        stopPlayback();
    }
    if (musicTracks[index].sound) {
        musicTracks[index].sound->release();
        musicTracks[index].sound = nullptr;
    }
    musicTracks.erase(musicTracks.begin() + index);

    if (currentTrackIndex >= (int)musicTracks.size()) {
        currentTrackIndex = -1;
    }
    return true;
}

bool PlaybackManager::playTrack(size_t index, bool fadeIn, unsigned int offsetMs)
{
    TSM_LOCK(PLAYBACK, "PlaybackResource");
    if (index >= musicTracks.size()) return false;

    if (isPlaying()) {
        stopPlayback();
    }

    if (!channelManager.startPlayback(musicTracks[index].sound, fadeIn ? 0.0f : volume)) {
        return false;
    }
    currentTrackIndex = (int)index;
    segmentStartTime = SDL_GetTicks();

    if (offsetMs > 0 && offsetMs < musicTracks[index].lengthMs) {
        FMOD::Channel* ch = channelManager.getCurrentChannel();
        if (ch) {
            ch->setPosition(offsetMs, FMOD_TIMEUNIT_MS);
        }
    }

    lastTrackIndex = index;
    lastSegmentOffset = offsetMs;

    if (fadeIn) {
        TSM_LOCK(PLAYBACK, "PlaybackResource");
        isFadingIn = true;
        fadeStartTime = SDL_GetTicks();
    }
    return true;
}

bool PlaybackManager::playLastSegment()
{
    TSM_LOCK(PLAYBACK, "PlaybackResource");
    if (lastTrackIndex >= musicTracks.size()) return false;
    return playTrack(lastTrackIndex, /*fadeIn=*/false, lastSegmentOffset);
}

bool PlaybackManager::stopPlayback()
{
    {
        TSM_LOCK(PLAYBACK, "PlaybackResource");
        isFadingOut = false;
        isFadingIn = false;
    }
    currentTrackIndex = -1;
    bool ok1 = channelManager.stopPlayback();
    bool ok2 = nextChannelManager.stopPlayback();
    return ok1 && ok2;
}

bool PlaybackManager::togglePause()
{
    if (!channelManager.isPlaying()) return false;

    bool wasPaused = channelManager.isPaused();
    if (!channelManager.setPaused(!wasPaused)) return false;

    if (nextChannelManager.isPlaying()) {
        if (!nextChannelManager.setPaused(!wasPaused)) return false;
    }
    return true;
}

void PlaybackManager::setVolume(float newVolume)
{
    volume = newVolume;
    TSM_LOCK(PLAYBACK, "PlaybackResource");
    if (!isFadingOut && !isFadingIn) {
        channelManager.setVolume(volume);
    }
}

unsigned int PlaybackManager::getPosition() const
{
    unsigned int pos = 0;
    channelManager.getPosition(&pos);
    return pos;
}

PlaybackManager::ChannelInfo PlaybackManager::getChannelInfo() const
{
    PlaybackManager::ChannelInfo info{ false, 0, 0.0f };
    FMOD::Channel* ch = channelManager.getCurrentChannel();
    if (ch) {
        ch->isPlaying(&info.isPlaying);
        ch->getPosition(&info.position, FMOD_TIMEUNIT_MS);
        ch->getVolume(&info.volume);
    }
    return info;
}

size_t PlaybackManager::getNextTrackIndex()
{
    TSM_LOCK(PLAYBACK, "PlaybackResource");
    if (musicTracks.empty()) return 0;

    if (randomOrder) {
        if (musicTracks.size() == 1) return 0;
        std::uniform_int_distribution<size_t> dist(0, musicTracks.size() - 1);
        size_t nextIdx;
        do {
            nextIdx = dist(rng);
        } while (nextIdx == (size_t)currentTrackIndex);
        return nextIdx;
    } else {
        return ((size_t)currentTrackIndex + 1) % musicTracks.size();
    }
}

void PlaybackManager::startFadeOut()
{
    if (isFadingOut || musicTracks.empty()) return;

    size_t nextIndex = getNextTrackIndex();
    if (nextIndex >= musicTracks.size()) return;

    {
        TSM_LOCK(PLAYBACK, "PlaybackResource");

        if (!nextChannelManager.startPlayback(musicTracks[nextIndex].sound, 0.0f)) {
            spdlog::warn("[PlaybackManager] Impossible de démarrer nextChannel");
            return;
        }
        
        if (randomSegment) {
            unsigned int soundLen = musicTracks[nextIndex].lengthMs;
            if (soundLen > 0) {
                if (segmentDuration >= (int)soundLen) {
                }
                else {
                    unsigned int maxOffset = soundLen - segmentDuration;
                    std::uniform_int_distribution<unsigned int> dist(0, maxOffset);
                    unsigned int startPos = dist(rng);

                    FMOD::Channel* nextCh = nextChannelManager.getCurrentChannel();
                    if (nextCh) {
                        nextCh->setPosition(startPos, FMOD_TIMEUNIT_MS);
                    }
                }
            }
        }
    }

    isFadingOut = true;
    isFadingIn  = true;
    fadeStartTime = SDL_GetTicks();
}

void PlaybackManager::update()
{
    {
        TSM_LOCK(PLAYBACK, "PlaybackResource");
        bool mainPlaying = channelManager.isPlaying();
        bool nextPlaying = nextChannelManager.isPlaying();

        if (!mainPlaying && !nextPlaying && !isFadingOut && !isFadingIn) {
            return;
        }
    }

    ChannelInfo info = getChannelInfo(); 
    
    unsigned int soundLength = 0;
    if (currentTrackIndex >= 0 && (size_t)currentTrackIndex < musicTracks.size()) {
        soundLength = musicTracks[currentTrackIndex].lengthMs;
    }

    Uint32 currentTime = SDL_GetTicks();
    Uint32 elapsedTime = currentTime - segmentStartTime;

    bool nearSongEnd = false;
    bool nearSegmentEnd = false;

    if (soundLength > 0 && (info.position + fadeDuration) >= soundLength) {
        nearSongEnd = true;
    }

    if (randomSegment) {
        bool segOK = (soundLength > 0 && (int)soundLength > segmentDuration);
        if (segOK && (elapsedTime + fadeDuration) >= (Uint32)segmentDuration) {
            nearSegmentEnd = true;
        }
    }

    if (!isFadingOut && (nearSongEnd || nearSegmentEnd)) {
        startFadeOut();
    }

    internalUpdateFade();
}


void PlaybackManager::internalUpdateFade()
{
    TSM_LOCK(PLAYBACK, "PlaybackResource");
    if (!isFadingOut && !isFadingIn) return;

    Uint32 now = SDL_GetTicks();
    float fadeProgress = float(now - fadeStartTime) / float(fadeDuration);
    fadeProgress = std::clamp(fadeProgress, 0.0f, 1.0f);

    FMOD::Channel* curCh  = channelManager.getCurrentChannel();
    FMOD::Channel* nextCh = nextChannelManager.getCurrentChannel();

    if (isFadingOut) {
        if (curCh) {
            float curVol = volume * (1.0f - fadeProgress);
            curCh->setVolume(curVol);
        }
        if (nextCh) {
            float nextVol = volume * fadeProgress;
            nextCh->setVolume(nextVol);
        }
        if (fadeProgress >= 1.0f) {
            channelManager.stopPlayback();

            std::swap(channelManager, nextChannelManager);
            nextChannelManager.stopPlayback();

            isFadingOut = false;
            isFadingIn  = false;

            size_t newIndex = getNextTrackIndex();
            currentTrackIndex = (int)newIndex;
            segmentStartTime = now;
        }
    }
    else if (isFadingIn) {
        if (curCh) {
            float curVol = volume * fadeProgress;
            curCh->setVolume(curVol);
        }
        if (fadeProgress >= 1.0f) {
            isFadingIn = false;
        }
    }
}


FMOD::Channel* getAudibleMusicChannel(PlaybackManager& pb) 
{
    FMOD::Channel* mainCh = pb.getCurrentChannel();
    FMOD::Channel* nextCh = pb.getNextChannelManager().getCurrentChannel(); 

    bool playingMain = false;
    bool playingNext = false;
    if(mainCh) mainCh->isPlaying(&playingMain);
    if(nextCh) nextCh->isPlaying(&playingNext);

    if(playingMain) return mainCh;
    if(playingNext) return nextCh;
    return nullptr;
}