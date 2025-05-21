// tsm_ui_manager.h

#pragma once

#include <SDL.h>
#include <string>
#include <vector>
#include <optional>

#include "tsm_playlist_manager.h"

namespace TSM
{

class UIManager
{
public:
    enum class WeddingPhase1State {
        IDLE,
        FADING_OUT_PREVIOUS,
        PLAYING_SFX_BEFORE,
        WAITING_AFTER_SFX,
        DUCKING_IN,
        PLAYING_ENTRANCE,
        DUCKING_OUT
    };

    static UIManager& GetInstance()
    {
        static UIManager instance;
        return instance;
    }

    bool Init(int width, int height);
    bool HandleEvents();
    void PreRender();
    void Render();
    void PostRender();
    void Shutdown();

    bool IsRunning() const { return m_isRunning; }
    bool IsInitialized() const { return m_isInitialized; }

    float GetMasterVolume() const        { return m_masterVolume; }
    float GetMusicVolume() const         { return m_musicVolume; }
    float GetAnnouncementVolume() const  { return m_announcementVolume; }
    float GetSFXVolume() const           { return m_sfxVolume; }

    void SetMasterVolume(float volume)        { m_masterVolume = volume; }
    void SetMusicVolume(float volume)         { m_musicVolume = volume; }
    void SetAnnouncementVolume(float volume)  { m_announcementVolume = volume; }
    void SetSFXVolume(float volume)           { m_sfxVolume = volume; }

    void ForceUpdateAllVolumes() { UpdateAllVolumes(); }

    void SetDuckFactor(float factor)   { m_duckFactor = factor; }
    float GetDuckFactor() const        { return m_duckFactor; }

    void UpdateWeddingMode(float deltaTime);
    
    void UpdateWeddingFilePaths();

    void PlayRandomMusic();
    void StartWeddingPhase1(bool transitionToNormalMusicAfter = false);
    void StartWeddingPhase2();
    void StartWeddingPhase3();
    void NextWeddingPhase();
    void StopAllMusic();

private:
    UIManager();
    ~UIManager() = default;

    UIManager(const UIManager&) = delete;
    UIManager& operator=(const UIManager&) = delete;

    void SetupBlenderStyle();

    void RenderPlaylistControls();
    void RenderAnnouncementControls();
    void RenderAudioControls();
    void RenderDebugInfo();

    void RenderAudioLibrary();
    void RenderMusicPlaylistTab();
    void RenderPlaylistManagerTab();
    void RenderAnnouncementsTab();
    void RenderSFXTab();
    void RenderWeddingModeTab();

    void UpdateAllVolumes();
    void CheckWeddingPhaseTransition();
    
    bool ImportWeddingMusic(int phase, const std::string& filePath);
    void StartNormalMusicAfterWedding();

    struct AudioTrack {
        std::string id;
        std::string name;
        bool isPlaying;
    };

    struct PlaylistData {
        std::string name;
        std::vector<AudioTrack> tracks;
        bool isPlaying;
    };

    void EnsureDefaultPlaylist();
    std::optional<PlaylistData> GetCurrentPlaylistData() const;
    std::string GetDisplayName(const std::string& path) const;

private:
    SDL_Window*   m_window     = nullptr;
    SDL_GLContext m_glContext  = nullptr;
    SDL_Renderer* m_renderer   = nullptr;
    bool          m_isRunning  = true;
    bool          m_isInitialized = false;

    float m_masterVolume       = 0.5f;
    float m_musicVolume        = 0.5f;
    float m_announcementVolume = 0.5f;
    float m_sfxVolume          = 0.5f;

    float m_duckFactor         = 1.0f;
    
    bool m_weddingModeActive = false;
    int m_weddingPhase = 0;  
    bool m_autoDuckingActive = false;
    float m_originalDuckFactor = 1.0f;
    float m_targetDuckFactor = 0.3f;
    float m_crossfadeDuration = 10.0f;
    bool m_autoTransitionToPhase2 = false;
    bool m_transitionToNormalMusicAfterWedding = false;
    
    std::string m_weddingEntranceFilePath;
    std::string m_weddingCeremonyFilePath;
    std::string m_weddingExitFilePath;
    
    std::string m_weddingEntranceSoundId = "wedding_entrance_sound";
    std::string m_weddingCeremonySoundId = "wedding_ceremony_sound";
    std::string m_weddingExitSoundId = "wedding_exit_sound";
    
    std::string m_normalPlaylistAfterWedding = "playlist_after_wedding";

    
    PlaylistOptions m_opts;
    std::string m_playlistName = "playlist_sample";

    WeddingPhase1State m_phase1State = WeddingPhase1State::IDLE;
    float m_phase1DuckTimer = 0.0f;
    float m_phase1DuckFadeDuration = 20.0f;
    float m_phase1FadeOutDuration = 5.0f;
    float m_phase1WaitDuration = 6.0f;
    FMOD::Channel* m_phase1SfxChannel = nullptr;
    FMOD::Channel* m_phase1EntranceChannel = nullptr;
    std::string m_phase1SfxName = "sfx_shine";
};

} // namespace TSM
