#include "tsm_runtime.h"

#include "tsm_announcement_manager.h"
#include "tsm_audio_manager.h"
#include "tsm_bluetooth_server.h"
#include "tsm_config.h"
#include "tsm_fmod_wrapper.h"
#include "tsm_mixer.h"
#include "tsm_playlist_manager.h"
#include "tsm_ui_manager.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace TSM
{
namespace
{

std::filesystem::path PathFromUtf8(const std::string& value)
{
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::filesystem::path AbsoluteNormalized(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

bool IsRegularFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::string ResolveAssetPath(
    const std::string& pathValue, const std::filesystem::path& resourceRoot)
{
    std::filesystem::path path = PathFromUtf8(pathValue);
    if (path.is_relative()) path = resourceRoot / path;
    return PathToUtf8(path.lexically_normal());
}

std::string ResolveApplicationDataFilePath(const std::string& fileName)
{
    const std::filesystem::path relativeFile = PathFromUtf8(fileName);
    if (relativeFile.empty() || relativeFile.is_absolute() ||
        relativeFile.has_parent_path() || relativeFile.filename() != relativeFile)
    {
        throw std::invalid_argument(
            "Application data file name must be a non-empty base name.");
    }

    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 1)
    {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required));
        const DWORD written = GetEnvironmentVariableW(
            L"LOCALAPPDATA", buffer.data(), required);
        if (written > 0 && written < required)
        {
            candidates.push_back(
                std::filesystem::path(buffer.data()) / "TheaterSoundManager");
        }
    }
#endif

    std::error_code temporaryError;
    const std::filesystem::path temporaryRoot =
        std::filesystem::temp_directory_path(temporaryError);
    if (!temporaryError)
        candidates.push_back(temporaryRoot / "TheaterSoundManager");

    for (const std::filesystem::path& directory : candidates)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) continue;
        if (!std::filesystem::is_directory(directory, error) || error) continue;
        return PathToUtf8((directory / relativeFile).lexically_normal());
    }

    throw std::runtime_error(
        "Unable to resolve a writable Theater Sound Manager data directory.");
}

std::filesystem::path ResolveStateDirectory(const std::string& configuredPath)
{
    std::filesystem::path directory;
    if (configuredPath.empty())
    {
        directory = PathFromUtf8(
            ResolveApplicationDataFilePath("state-directory-probe")).parent_path();
    }
    else
    {
        directory = AbsoluteNormalized(PathFromUtf8(configuredPath));
        std::error_code createError;
        std::filesystem::create_directories(directory, createError);
        if (createError)
            throw std::runtime_error(createError.message());
    }

    std::error_code directoryError;
    if (!std::filesystem::is_directory(directory, directoryError) || directoryError)
        throw std::runtime_error(
            directoryError ? directoryError.message() : "Path is not a directory.");
    return directory.lexically_normal();
}

std::string FingerprintFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
        throw std::runtime_error("Unable to open configuration for fingerprinting.");

    std::uint64_t hash = 14695981039346656037ULL;
    std::array<char, 16384> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index)
        {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= 1099511628211ULL;
        }
    }
    if (input.bad())
        throw std::runtime_error("Unable to read configuration for fingerprinting.");

    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int index = 15; index >= 0; --index)
    {
        result[static_cast<std::size_t>(index)] = digits[hash & 0x0fU];
        hash >>= 4U;
    }
    return result;
}

std::string FormatValidationError(
    const std::string& parserError,
    const std::vector<ConfigValidationIssue>& issues)
{
    if (!parserError.empty()) return parserError;
    std::ostringstream output;
    output << "Configuration validation failed";
    for (const auto& issue : issues)
    {
        output << "\n- " << issue.path << ": " << issue.message;
    }
    return output.str();
}

const char* BluetoothStateName(BluetoothServerState state)
{
    switch (state)
    {
        case BluetoothServerState::Stopped: return "stopped";
        case BluetoothServerState::Starting: return "starting";
        case BluetoothServerState::Running: return "running";
        case BluetoothServerState::Failed: return "failed";
    }
    return "failed";
}

} // namespace

std::string GetApplicationDataFilePath(const std::string& fileName)
{
    return ResolveApplicationDataFilePath(fileName);
}

ApplicationRuntime::~ApplicationRuntime()
{
    Shutdown();
}

bool ApplicationRuntime::Initialize(const RuntimeOptions& options, std::string& errorMessage)
{
    errorMessage.clear();
    if (m_initialized) return true;

    m_lastErrorKind = RuntimeErrorKind::None;
    m_configPath.clear();
    m_resourceRoot.clear();
    m_stateDirectory.clear();
    m_loadReport = {};
    m_bluetoothEnabled = false;
    m_bluetoothStarted = false;
    m_bluetoothState = options.startBluetooth ? "starting" : "disabled";
    m_bluetoothError.clear();
    m_noSound = options.noSound;

    try
    {
        m_stateDirectory = PathToUtf8(
            ResolveStateDirectory(options.stateDirectory));
    }
    catch (const std::exception& pathError)
    {
        m_lastErrorKind = RuntimeErrorKind::InputOutput;
        errorMessage = std::string("Unable to initialize the state directory: ") +
            pathError.what();
        return false;
    }

    if (!FModWrapper::GetInstance().Initialize(options.noSound))
    {
        m_lastErrorKind = RuntimeErrorKind::AudioEngine;
        errorMessage = "Unable to initialize the FMOD audio engine.";
        return false;
    }

    m_initialized = true;
    MixerState::GetInstance().Reset();
    try
    {
        AudioManager::GetInstance().ConfigureLoudness(
            PathToUtf8(
                (PathFromUtf8(m_stateDirectory) / "loudness_cache.json").lexically_normal()),
            -16.0f);
    }
    catch (const std::exception& pathError)
    {
        m_lastErrorKind = RuntimeErrorKind::InputOutput;
        errorMessage = std::string("Unable to initialize application data: ") +
            pathError.what();
        Shutdown();
        return false;
    }

    if (options.loadConfig && !LoadConfiguration(options.configPath, errorMessage))
    {
        Shutdown();
        return false;
    }

    if (options.startBluetooth)
    {
        std::string bluetoothError;
        if (StartBluetoothServer(bluetoothError))
        {
            m_bluetoothStarted = true;
            m_bluetoothEnabled = true;
            m_bluetoothState = "running";
        }
        else
        {
            m_bluetoothState = "failed";
            m_bluetoothError = std::move(bluetoothError);
            spdlog::warn("Bluetooth server is unavailable: {}", m_bluetoothError);
        }
    }
    return true;
}

bool ApplicationRuntime::ReloadConfiguration(
    const std::string& configPath, std::string& errorMessage)
{
    if (!m_initialized)
    {
        errorMessage = "Runtime is not initialized.";
        return false;
    }
    return LoadConfiguration(configPath, errorMessage);
}

bool ApplicationRuntime::LoadConfiguration(
    const std::string& configPath, std::string& errorMessage)
{
    if (m_cinemaManager && !m_cinemaManager->IsReloadAllowed())
    {
        m_lastErrorKind = RuntimeErrorKind::Configuration;
        errorMessage =
            "Configuration reload is blocked while a safety incident is latched.";
        return false;
    }
    const bool preserveScheduleState = !m_configPath.empty();
    const auto previousSchedules = preserveScheduleState
        ? AnnouncementManager::GetInstance().GetScheduledAnnouncements()
        : std::vector<AnnouncementManager::ScheduledAnnouncement>{};
    const std::filesystem::path absoluteConfig = AbsoluteNormalized(PathFromUtf8(configPath));
    const std::string absoluteConfigUtf8 = PathToUtf8(absoluteConfig);

    std::vector<ConfigValidationIssue> issues;
    std::string validationError;
    AppConfig config;
    if (!LoadValidatedAppConfig(
            absoluteConfigUtf8, config, issues, validationError))
    {
        m_lastErrorKind = RuntimeErrorKind::Configuration;
        errorMessage = FormatValidationError(validationError, issues);
        return false;
    }

    std::string configFingerprint;
    try
    {
        configFingerprint = FingerprintFile(absoluteConfig);
    }
    catch (const std::exception& fingerprintError)
    {
        m_lastErrorKind = RuntimeErrorKind::InputOutput;
        errorMessage = fingerprintError.what();
        return false;
    }

    if (m_cinemaManager)
    {
        m_cinemaManager->Shutdown();
        m_cinemaManager.reset();
    }
    ResetSession();
    m_loadReport = {};
    m_configPath = absoluteConfigUtf8;

    std::filesystem::path resourceRoot = absoluteConfig.parent_path();
    if (resourceRoot.filename() == "config") resourceRoot = resourceRoot.parent_path();
    resourceRoot = resourceRoot.lexically_normal();
    m_resourceRoot = PathToUtf8(resourceRoot);

    auto& audioManager = AudioManager::GetInstance();
    auto& playlistManager = PlaylistManager::GetInstance();
    auto& announcementManager = AnnouncementManager::GetInstance();
    std::set<std::string> cinemaPlaylistNames;
    for (const auto& schedule : config.cinema.schedules)
        cinemaPlaylistNames.insert(schedule.playlist);
    bool criticalCinemaAssetFailure = false;
    for (const auto& previous : previousSchedules)
        announcementManager.SetNextScheduleIdAtLeast(previous.scheduleId + 1);
    try
    {
        audioManager.ConfigureLoudness(
            PathToUtf8(
                (PathFromUtf8(m_stateDirectory) / "loudness_cache.json").lexically_normal()),
            config.loudnessTargetLufs);
    }
    catch (const std::exception& pathError)
    {
        m_lastErrorKind = RuntimeErrorKind::InputOutput;
        errorMessage = std::string("Unable to initialize application data: ") +
            pathError.what();
        return false;
    }

    for (const auto& playlist : config.playlists)
    {
        playlistManager.CreatePlaylist(playlist.name);
        ++m_loadReport.playlists;
        for (const auto& track : playlist.tracks)
        {
            const std::string path = ResolveAssetPath(track.path, resourceRoot);
            const bool alreadyLoaded = audioManager.GetAllSounds().contains(track.id);
            if (audioManager.LoadSound(
                    track.id, path, true, AudioManager::SoundKind::Music))
            {
                if (!alreadyLoaded) ++m_loadReport.sounds;
                bool usable = true;
                if (cinemaPlaylistNames.contains(playlist.name))
                {
                    unsigned int durationMilliseconds = 0;
                    FMOD::Sound* sound = audioManager.GetSound(track.id);
                    usable = sound &&
                        sound->getLength(
                            &durationMilliseconds, FMOD_TIMEUNIT_MS) == FMOD_OK &&
                        durationMilliseconds > 0;
                }
                if (usable)
                {
                    playlistManager.AddToPlaylist(playlist.name, track.id);
                }
                else
                {
                    m_loadReport.failures.push_back(
                        {track.id, path,
                         "Cinema playlist track has no playable duration."});
                    criticalCinemaAssetFailure = true;
                }
            }
            else
            {
                m_loadReport.failures.push_back(
                    {track.id, path, "Unable to load playlist track."});
                if (cinemaPlaylistNames.contains(playlist.name))
                    criticalCinemaAssetFailure = true;
            }
        }
        if (auto* loadedPlaylist = playlistManager.GetPlaylistByName(playlist.name))
            loadedPlaylist->options = playlist.options;
    }

    for (const auto& announcement : config.announcements)
    {
        const std::string path = ResolveAssetPath(announcement.path, resourceRoot);
        const bool emergencyPreset = config.cinema.safety.enabled &&
            announcement.id == config.cinema.safety.evacuationAnnouncementId;
        const bool loaded = emergencyPreset
            ? audioManager.LoadEmergencySound(announcement.id, path)
            : audioManager.LoadAnnouncement(announcement.id, path);
        if (!loaded)
        {
            m_loadReport.failures.push_back(
                {announcement.id, path,
                 emergencyPreset
                    ? "Unable to load protected evacuation preset."
                    : "Unable to load announcement."});
            if (emergencyPreset) criticalCinemaAssetFailure = true;
            continue;
        }
        ++m_loadReport.announcements;
        if (emergencyPreset)
        {
            unsigned int durationMilliseconds = 0;
            FMOD::Sound* sound = audioManager.GetSound(announcement.id);
            if (!sound ||
                sound->getLength(
                    &durationMilliseconds, FMOD_TIMEUNIT_MS) != FMOD_OK ||
                durationMilliseconds == 0)
            {
                m_loadReport.failures.push_back(
                    {announcement.id, path,
                     "Protected evacuation preset has no playable duration."});
                criticalCinemaAssetFailure = true;
            }
        }
        if (!emergencyPreset && announcement.hour >= 0 && announcement.minute >= 0)
        {
            const auto previous = std::find_if(
                previousSchedules.begin(), previousSchedules.end(),
                [&](const auto& schedule) {
                    return schedule.hour == announcement.hour &&
                           schedule.minute == announcement.minute &&
                           schedule.announcementId == announcement.id;
                });
            if (previous != previousSchedules.end())
                announcementManager.RestoreScheduledAnnouncement(*previous);
            else
                announcementManager.ScheduleAnnouncement(
                    announcement.hour, announcement.minute, announcement.id);
            ++m_loadReport.schedules;
        }
    }

    if (criticalCinemaAssetFailure)
    {
        m_lastErrorKind = RuntimeErrorKind::Configuration;
        errorMessage =
            "A required cinema playlist or evacuation asset could not be loaded.";
        ResetSession();
        AudioManager::GetInstance().SetNormalPlaybackBlocked(true);
        return false;
    }

    m_cinemaManager = std::make_unique<CinemaManager>();
    std::string cinemaError;
    if (!m_cinemaManager->Initialize(
            config.cinema,
            PathFromUtf8(m_stateDirectory),
            configFingerprint,
            cinemaError))
    {
        m_lastErrorKind = RuntimeErrorKind::Configuration;
        errorMessage = "Unable to initialize cinema operation: " + cinemaError;
        m_cinemaManager.reset();
        ResetSession();
        AudioManager::GetInstance().SetNormalPlaybackBlocked(true);
        return false;
    }

    UIManager::GetInstance().RefreshPlaylistSelection();
    if (!m_loadReport.failures.empty())
    {
        spdlog::warn(
            "Configuration loaded with {} unavailable asset(s).",
            m_loadReport.failures.size());
    }
    return true;
}

void ApplicationRuntime::Tick(float deltaTime)
{
    if (!m_initialized) return;
    if (m_cinemaManager) m_cinemaManager->Tick();
    const bool normalAudioAllowed = CanPlayNormalAudio();
    if (m_bluetoothStarted)
    {
        const BluetoothServerStatus status = GetBluetoothServerStatus();
        m_bluetoothState = BluetoothStateName(status.state);
        m_bluetoothError = status.error;
        m_bluetoothEnabled = status.state == BluetoothServerState::Running;
        if (m_bluetoothEnabled && normalAudioAllowed)
            ProcessPendingBluetoothCommands();
        else if (!normalAudioAllowed)
            DiscardPendingBluetoothCommands();
    }
    AudioManager::GetInstance().Update(deltaTime);
    if (normalAudioAllowed)
    {
        AnnouncementManager::GetInstance().Update(deltaTime);
        PlaylistManager::GetInstance().Update(deltaTime);
    }
}

void ApplicationRuntime::ResetSession()
{
    UIManager::GetInstance().StopAllMusic();
    AnnouncementManager::GetInstance().ResetSession();

    auto& playlistManager = PlaylistManager::GetInstance();
    const auto playlistNames = playlistManager.GetPlaylistNames();
    for (const auto& name : playlistNames) playlistManager.DeletePlaylist(name);

    auto& audioManager = AudioManager::GetInstance();
    std::vector<std::string> soundNames;
    soundNames.reserve(audioManager.GetAllSounds().size());
    for (const auto& [name, data] : audioManager.GetAllSounds())
    {
        (void)data;
        soundNames.push_back(name);
    }
    for (const auto& name : soundNames) audioManager.UnloadSound(name);
    UIManager::GetInstance().ResetSessionState();
}

void ApplicationRuntime::Shutdown()
{
    if (!m_initialized) return;
    if (m_bluetoothStarted)
    {
        StopBluetoothServer();
        m_bluetoothStarted = false;
        m_bluetoothEnabled = false;
        m_bluetoothState = "stopped";
        m_bluetoothError.clear();
    }
    if (m_cinemaManager)
    {
        m_cinemaManager->Shutdown();
        m_cinemaManager.reset();
    }
    ResetSession();
    AudioManager::GetInstance().Shutdown();
    FModWrapper::GetInstance().Shutdown();
    m_initialized = false;
    m_stateDirectory.clear();
}

std::string GetExecutablePath(const std::string& fallbackPath)
{
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size())
    {
        const int utf8Length = WideCharToMultiByte(
            CP_UTF8, 0, buffer.data(), static_cast<int>(length),
            nullptr, 0, nullptr, nullptr);
        if (utf8Length > 0)
        {
            std::string result(static_cast<std::size_t>(utf8Length), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, buffer.data(), static_cast<int>(length),
                result.data(), utf8Length, nullptr, nullptr);
            return result;
        }
    }
#endif
    return fallbackPath;
}

std::string ResolveDefaultConfigPath(const std::string& executablePath)
{
    const std::filesystem::path executable = AbsoluteNormalized(PathFromUtf8(executablePath));
    const std::filesystem::path executableDirectory = executable.parent_path();
    const std::filesystem::path installedCandidate =
        executableDirectory / "config" / "tsm_config.json";
    if (IsRegularFile(installedCandidate))
        return PathToUtf8(installedCandidate.lexically_normal());

    const std::filesystem::path developmentCandidate =
        executableDirectory / ".." / ".." / ".." / "config" / "tsm_config.json";
    if (IsRegularFile(developmentCandidate))
        return PathToUtf8(AbsoluteNormalized(developmentCandidate));

    return PathToUtf8(installedCandidate.lexically_normal());
}

} // namespace TSM
