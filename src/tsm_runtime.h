#pragma once

#include "tsm_cinema_manager.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace TSM
{

struct RuntimeOptions
{
    std::string configPath = "config/tsm_config.json";
    std::string stateDirectory;
    bool loadConfig = true;
    bool startBluetooth = false;
    bool noSound = false;
};

enum class RuntimeErrorKind
{
    None,
    Configuration,
    AudioEngine,
    InputOutput
};

struct AssetLoadFailure
{
    std::string id;
    std::string path;
    std::string reason;
};

struct AssetLoadReport
{
    std::size_t playlists = 0;
    std::size_t sounds = 0;
    std::size_t announcements = 0;
    std::size_t schedules = 0;
    std::vector<AssetLoadFailure> failures;
};

class ApplicationRuntime
{
public:
    ApplicationRuntime() = default;
    ~ApplicationRuntime();

    ApplicationRuntime(const ApplicationRuntime&) = delete;
    ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;

    bool Initialize(const RuntimeOptions& options, std::string& errorMessage);
    bool ReloadConfiguration(const std::string& configPath, std::string& errorMessage);
    void Tick(float deltaTime);
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    bool IsBluetoothEnabled() const { return m_bluetoothEnabled; }
    const std::string& GetBluetoothState() const { return m_bluetoothState; }
    const std::string& GetBluetoothError() const { return m_bluetoothError; }
    bool IsNoSound() const { return m_noSound; }
    const std::string& GetConfigPath() const { return m_configPath; }
    const std::string& GetResourceRoot() const { return m_resourceRoot; }
    const std::string& GetStateDirectory() const { return m_stateDirectory; }
    const AssetLoadReport& GetLoadReport() const { return m_loadReport; }
    RuntimeErrorKind GetLastErrorKind() const { return m_lastErrorKind; }
    CinemaManager* GetCinemaManager() { return m_cinemaManager.get(); }
    const CinemaManager* GetCinemaManager() const { return m_cinemaManager.get(); }
    bool CanPlayNormalAudio() const
    {
        return !m_cinemaManager || m_cinemaManager->CanPlayNormalAudio();
    }

private:
    bool LoadConfiguration(const std::string& configPath, std::string& errorMessage);
    void ResetSession();

    bool m_initialized = false;
    bool m_bluetoothEnabled = false;
    bool m_bluetoothStarted = false;
    bool m_noSound = false;
    std::string m_bluetoothState = "disabled";
    std::string m_bluetoothError;
    std::string m_configPath;
    std::string m_resourceRoot;
    std::string m_stateDirectory;
    AssetLoadReport m_loadReport;
    RuntimeErrorKind m_lastErrorKind = RuntimeErrorKind::None;
    std::unique_ptr<CinemaManager> m_cinemaManager;
};

std::string GetExecutablePath(const std::string& fallbackPath);
std::string ResolveDefaultConfigPath(const std::string& executablePath);
std::string GetApplicationDataFilePath(const std::string& fileName);

} // namespace TSM
