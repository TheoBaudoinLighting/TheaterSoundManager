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
    float GetDuckFactor() const          { return MixerState::GetInstance().GetDuckFactor(); }

    void SetMasterVolume(float volume)        { MixerState::GetInstance().SetMasterVolume(volume); }
    void SetMusicVolume(float volume)         { MixerState::GetInstance().SetMusicVolume(volume); }
    void SetAnnouncementVolume(float volume)  { MixerState::GetInstance().SetAnnouncementVolume(volume); }
    void SetSFXVolume(float volume)            { MixerState::GetInstance().SetSfxVolume(volume); }
    void SetDuckFactor(float factor)            { MixerState::GetInstance().SetDuckFactor(factor); }

    void ForceUpdateAllVolumes() { UpdateAllVolumes(); }

    void PlayRandomMusic();
    void StopAllMusic();
    void ResetSessionState();
    void RefreshPlaylistSelection();
    const std::string& GetSelectedPlaylistName() const { return m_playlistName; }

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

    void UpdateAllVolumes();

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

    float m_crossfadeDuration = 10.0f;
    PlaylistOptions m_opts;
    std::string m_playlistName;
    std::string m_playlistFeedback;
    bool m_playlistFeedbackIsError = false;

    // Persistent ImGui draft for the playlist editor. Slider edits span
    // multiple frames and are committed only on release, so rebuilding this
    // state from the playlist every frame would discard the released value.
    bool m_playlistSettingsDraftValid = false;
    std::string m_playlistSettingsDraftName;
    PlaylistOptions m_playlistSettingsDraftOptions;
    float m_playlistSettingsDraftCrossfade = 10.0f;

};

} // namespace TSM
