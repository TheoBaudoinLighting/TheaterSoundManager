// tsm_ui_manager.h

#pragma once

#include <SDL.h>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

#include "tsm_mixer.h"
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

    bool Init(
        int width,
        int height,
        const std::string& resourceRoot = {},
        const std::string& imguiIniPath = {});
    bool HandleEvents();
    void PreRender();
    void Render();
    void PostRender();
    void Shutdown();

    bool IsRunning() const { return m_isRunning; }
    bool IsInitialized() const { return m_isInitialized; }

    float GetMasterVolume() const        { return MixerState::GetInstance().GetMasterVolume(); }
    float GetMusicVolume() const         { return MixerState::GetInstance().GetMusicVolume(); }
    float GetAnnouncementVolume() const  { return MixerState::GetInstance().GetAnnouncementVolume(); }
    float GetSFXVolume() const           { return MixerState::GetInstance().GetSfxVolume(); }

    void SetMasterVolume(float volume)        { MixerState::GetInstance().SetMasterVolume(volume); }
    void SetMusicVolume(float volume)         { MixerState::GetInstance().SetMusicVolume(volume); }
    void SetAnnouncementVolume(float volume)  { MixerState::GetInstance().SetAnnouncementVolume(volume); }
    void SetSFXVolume(float volume)            { MixerState::GetInstance().SetSfxVolume(volume); }

    void ForceUpdateAllVolumes() { UpdateAllVolumes(); }

    void SetDuckFactor(float factor)   { MixerState::GetInstance().SetWeddingDuckFactor(factor); }
    float GetDuckFactor() const        { return MixerState::GetInstance().GetWeddingDuckFactor(); }

    void UpdateWeddingMode(float deltaTime);
    
    void UpdateWeddingFilePaths();

    void PlayRandomMusic();
    void StartWeddingPhase1(bool transitionToNormalMusicAfter = false);
    void StartWeddingPhase2(bool transitionToNormalMusicAfter = false);
    void StartWeddingPhase3(bool transitionToNormalMusicAfter = false, const std::string& postWeddingPlaylist = "");
    void NextWeddingPhase();
    void StopWeddingMode();
    void StopAllMusic();
    void ResetSessionState();
    void RefreshPlaylistSelection();
    const std::string& GetSelectedPlaylistName() const { return m_playlistName; }
    bool IsWeddingModeActive() const { return m_weddingModeActive; }
    int GetWeddingPhase() const { return m_weddingPhase; }
    const char* GetWeddingStateString() const;

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
    void StopAudioBeforeWeddingPhase();
    
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
    bool SelectPlaylist(const std::string& playlistName);
    std::optional<PlaylistData> GetCurrentPlaylistData() const;
    std::string GetDisplayName(const std::string& path) const;

private:
    SDL_Window*   m_window     = nullptr;
    SDL_GLContext m_glContext  = nullptr;
    SDL_Renderer* m_renderer   = nullptr;
    bool          m_isRunning  = true;
    bool          m_isInitialized = false;
    bool          m_sdlInitialized = false;
    bool          m_imguiContextCreated = false;
    bool          m_imguiSdlInitialized = false;
    bool          m_imguiOpenGlInitialized = false;
    std::string   m_imguiIniPath;

    bool m_weddingModeActive = false;
    int m_weddingPhase = 0;  
    bool m_autoDuckingActive = false;
    float m_originalDuckFactor = 1.0f;
    float m_targetDuckFactor = 0.3f;
    float m_crossfadeDuration = 10.0f;
    float m_autoDuckStartFactor = 1.0f;
    float m_autoDuckTimer = 0.0f;
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
    std::string m_playlistName;
    std::string m_playlistFeedback;
    bool m_playlistFeedbackIsError = false;

    WeddingPhase1State m_phase1State = WeddingPhase1State::IDLE;
    float m_phase1DuckTimer = 0.0f;
    float m_phase1DuckFadeDuration = 20.0f;
    float m_phase1FadeOutDuration = 5.0f;
    float m_phase1WaitDuration = 6.0f;
    float m_phasesTransitionDuration = 10.0f;  
    FMOD::Channel* m_phase1SfxChannel = nullptr;
    FMOD::Channel* m_phase1EntranceChannel = nullptr;
    FMOD::Channel* m_weddingCeremonyChannel = nullptr;
    FMOD::Channel* m_weddingExitChannel = nullptr;
    std::string m_phase1SfxName = "sfx_shine";
};

} // namespace TSM
