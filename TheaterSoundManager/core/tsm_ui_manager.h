// tsm_ui_manager.h

#pragma once

#include <SDL.h>
#include <string>
#include <vector>
#include <optional>

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
};

} // namespace TSM
