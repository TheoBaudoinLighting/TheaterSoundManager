// tsm_audio_manager.cpp

#include "tsm_audio_manager.h"
#include "tsm_fmod_wrapper.h"
#include "tsm_mixer.h"
#include "tsm_music_analysis_store.h"
#include <fmod_dsp_effects.h>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#endif
#include <utility>

namespace TSM 
{

namespace
{
std::filesystem::path PathFromUtf8(const std::string& filePath)
{
    const auto* begin = reinterpret_cast<const char8_t*>(filePath.data());
    return std::filesystem::path(std::u8string(begin, begin + filePath.size()));
}

std::string NormalizeCacheKey(const std::string& filePath)
{
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(PathFromUtf8(filePath), error);
    if (error) path = PathFromUtf8(filePath);
    const std::u8string normalized = path.lexically_normal().generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(normalized.data()), normalized.size());
}

std::uintmax_t GetFileSize(const std::string& filePath)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(PathFromUtf8(filePath), error);
    return error ? 0 : size;
}

std::int64_t GetFileWriteTime(const std::string& filePath)
{
    std::error_code error;
    const auto time = std::filesystem::last_write_time(PathFromUtf8(filePath), error);
    return error ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
}

bool SameFileIdentity(
    const MusicAnalysis::FileIdentity& left,
    const MusicAnalysis::FileIdentity& right)
{
    return !left.normalizedPath.empty() &&
        left.normalizedPath == right.normalizedPath &&
        left.size == right.size &&
        left.writeTime == right.writeTime;
}

bool MergePlaybackHeatmaps(
    const ListeningHeatmap::State& durable,
    const ListeningHeatmap::State& pending,
    ListeningHeatmap::State& merged)
{
    ListeningHeatmap::State durableState =
        ListeningHeatmap::ExportState(durable);
    ListeningHeatmap::State pendingState =
        ListeningHeatmap::ExportState(pending);
    if (std::abs(
            durableState.trackDurationSeconds -
            pendingState.trackDurationSeconds) > 0.1 ||
        durableState.bucketFatigue.size() !=
            pendingState.bucketFatigue.size() ||
        std::abs(
            durableState.bucketDurationSeconds -
            pendingState.bucketDurationSeconds) > 0.000001 ||
        std::abs(
            durableState.dailyDecayFactor -
            pendingState.dailyDecayFactor) > 0.000001)
    {
        return false;
    }

    const std::int64_t referenceEpochSeconds = std::max(
        durableState.referenceEpochSeconds,
        pendingState.referenceEpochSeconds);
    ListeningHeatmap::DecayTo(durableState, referenceEpochSeconds);
    ListeningHeatmap::DecayTo(pendingState, referenceEpochSeconds);
    for (std::size_t index = 0;
         index < durableState.bucketFatigue.size(); ++index)
    {
        durableState.bucketFatigue[index] = std::clamp(
            durableState.bucketFatigue[index] +
                pendingState.bucketFatigue[index],
            0.0,
            1.0);
    }
    merged = std::move(durableState);
    return true;
}

std::uint64_t HashIdentityPart(
    const MusicAnalysis::FileIdentity& identity,
    std::uint64_t seed)
{
    constexpr std::uint64_t Prime = 1099511628211ULL;
    auto appendByte = [&](unsigned char value) {
        seed ^= static_cast<std::uint64_t>(value);
        seed *= Prime;
    };
    for (const unsigned char value : identity.normalizedPath) appendByte(value);
    appendByte(0xffu);
    for (unsigned int shift = 0; shift < 64; shift += 8)
        appendByte(static_cast<unsigned char>(identity.size >> shift));
    appendByte(0xfeu);
    const std::uint64_t writeTime = static_cast<std::uint64_t>(identity.writeTime);
    for (unsigned int shift = 0; shift < 64; shift += 8)
        appendByte(static_cast<unsigned char>(writeTime >> shift));
    return seed;
}

std::string StableTransitionTrackId(
    const MusicAnalysis::FileIdentity& identity)
{
    if (identity.normalizedPath.empty()) return {};
    const std::uint64_t first = HashIdentityPart(
        identity, 14695981039346656037ULL);
    const std::uint64_t second = HashIdentityPart(
        identity, 7809847782465536322ULL);
    constexpr char Hex[] = "0123456789abcdef";
    std::string result = "pm1-";
    result.reserve(36);
    const auto appendHex = [&](std::uint64_t value, std::string& target) {
        for (int shift = 60; shift >= 0; shift -= 4)
            target.push_back(Hex[(value >> shift) & 0x0fULL]);
    };
    appendHex(first, result);
    appendHex(second, result);
    return result;
}

void BumpPlaybackMemoryVersion(std::uint64_t& version)
{
    ++version;
    // Zero is reserved for a state that has never been mutated. Unsigned
    // overflow is defined, and skipping zero keeps diagnostics unambiguous.
    if (version == 0) ++version;
}

void AppendPlaybackMemoryError(
    std::string& aggregate,
    const std::string& context,
    const std::string& detail)
{
    constexpr std::size_t MaximumDiagnosticBytes = 4096;
    if (aggregate.size() >= MaximumDiagnosticBytes) return;
    if (!aggregate.empty()) aggregate += " | ";
    aggregate += context;
    if (!detail.empty())
    {
        aggregate += ": ";
        aggregate += detail;
    }
    if (aggregate.size() > MaximumDiagnosticBytes)
    {
        aggregate.resize(MaximumDiagnosticBytes);
        aggregate += "...";
    }
}

struct LoudnessCacheEntry
{
    std::uintmax_t size = 0;
    std::int64_t writeTime = 0;
    float integratedLufs = 0.0f;
    float truePeakDb = 0.0f;
};

bool ReadUnsignedInteger(const nlohmann::json& value, std::uintmax_t& output)
{
    try
    {
        if (value.is_number_unsigned())
        {
            output = value.get<std::uintmax_t>();
            return true;
        }
        if (value.is_number_integer())
        {
            const std::int64_t signedValue = value.get<std::int64_t>();
            if (signedValue < 0) return false;
            output = static_cast<std::uintmax_t>(signedValue);
            return true;
        }
    }
    catch (const std::exception&)
    {
    }
    return false;
}

bool ReadSignedInteger(const nlohmann::json& value, std::int64_t& output)
{
    try
    {
        if (value.is_number_unsigned())
        {
            const std::uint64_t unsignedValue = value.get<std::uint64_t>();
            if (unsignedValue > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()))
                return false;
            output = static_cast<std::int64_t>(unsignedValue);
            return true;
        }
        if (value.is_number_integer())
        {
            output = value.get<std::int64_t>();
            return true;
        }
    }
    catch (const std::exception&)
    {
    }
    return false;
}

bool ReadFiniteFloat(const nlohmann::json& value, float& output)
{
    if (!value.is_number()) return false;
    try
    {
        const double number = value.get<double>();
        if (!std::isfinite(number) ||
            number < static_cast<double>(std::numeric_limits<float>::lowest()) ||
            number > static_cast<double>(std::numeric_limits<float>::max()))
            return false;
        output = static_cast<float>(number);
        return std::isfinite(output);
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ReadLoudnessCacheEntry(
    const nlohmann::json& value, LoudnessCacheEntry& entry)
{
    if (!value.is_object()) return false;
    const auto size = value.find("size");
    const auto writeTime = value.find("writeTime");
    const auto integratedLufs = value.find("integratedLufs");
    const auto truePeakDb = value.find("truePeakDb");
    if (size == value.end() || writeTime == value.end() ||
        integratedLufs == value.end() || truePeakDb == value.end())
        return false;

    if (!ReadUnsignedInteger(*size, entry.size) ||
        !ReadSignedInteger(*writeTime, entry.writeTime) ||
        !ReadFiniteFloat(*integratedLufs, entry.integratedLufs) ||
        !ReadFiniteFloat(*truePeakDb, entry.truePeakDb))
        return false;

    return entry.integratedLufs > -80.0f;
}

nlohmann::json LoadLoudnessCache(const std::string& cachePath)
{
    nlohmann::json sanitized = nlohmann::json::object();
    try
    {
        std::ifstream input(PathFromUtf8(cachePath), std::ios::binary);
        if (!input.is_open()) return sanitized;

        nlohmann::json raw;
        input >> raw;
        if (!raw.is_object())
        {
            spdlog::warn(
                "Ignoring loudness cache '{}': root value must be an object.", cachePath);
            return sanitized;
        }

        std::size_t rejectedEntries = 0;
        for (const auto& [key, value] : raw.items())
        {
            LoudnessCacheEntry entry;
            if (!ReadLoudnessCacheEntry(value, entry))
            {
                ++rejectedEntries;
                continue;
            }
            sanitized[key] = {
                {"size", entry.size},
                {"writeTime", entry.writeTime},
                {"integratedLufs", entry.integratedLufs},
                {"truePeakDb", entry.truePeakDb}
            };
        }
        if (rejectedEntries > 0)
        {
            spdlog::warn(
                "Ignored {} malformed loudness cache entr{} in '{}'.",
                rejectedEntries, rejectedEntries == 1 ? "y" : "ies", cachePath);
        }
    }
    catch (const std::exception& error)
    {
        spdlog::warn("Ignoring invalid loudness cache '{}': {}", cachePath, error.what());
        sanitized = nlohmann::json::object();
    }
    catch (...)
    {
        spdlog::warn("Ignoring invalid loudness cache '{}': unknown error.", cachePath);
        sanitized = nlohmann::json::object();
    }
    return sanitized;
}

bool SaveLoudnessCacheAtomically(
    const std::string& cachePath, const nlohmann::json& cache, std::string& errorMessage)
{
    std::filesystem::path temporaryPath;
    try
    {
        const std::filesystem::path targetPath = PathFromUtf8(cachePath);
        if (!targetPath.parent_path().empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(targetPath.parent_path(), directoryError);
            if (directoryError)
            {
                errorMessage = directoryError.message();
                return false;
            }
        }

        temporaryPath = targetPath;
#ifdef _WIN32
        temporaryPath += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
#else
        temporaryPath += ".tmp";
#endif
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);

        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                errorMessage = "unable to open temporary cache file";
                std::filesystem::remove(temporaryPath, ignored);
                return false;
            }
            output << cache.dump(2);
            output.flush();
            if (!output.good())
            {
                errorMessage = "unable to write temporary cache file";
                output.close();
                std::filesystem::remove(temporaryPath, ignored);
                return false;
            }
        }

#ifdef _WIN32
        if (!MoveFileExW(
                temporaryPath.c_str(), targetPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            errorMessage = std::error_code(
                static_cast<int>(GetLastError()), std::system_category()).message();
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }
#else
        std::error_code renameError;
        std::filesystem::rename(temporaryPath, targetPath, renameError);
        if (renameError)
        {
            errorMessage = renameError.message();
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }
#endif
        return true;
    }
    catch (const std::exception& error)
    {
        errorMessage = error.what();
    }
    catch (...)
    {
        errorMessage = "unknown error";
    }

    if (!temporaryPath.empty())
    {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
    }
    return false;
}

bool AnalyzeTrackAudio(FMOD::System* system, const std::string& filePath,
                       const std::atomic<bool>& stopRequested,
                       float& integratedLufs, float& truePeakDb,
                       MusicAnalysis::TrackAnalysis& musicAnalysis)
{
    FMOD::Sound* sound = nullptr;
    FMOD_RESULT result = system->createSound(
        filePath.c_str(), FMOD_2D | FMOD_CREATESTREAM, nullptr, &sound);
    if (result != FMOD_OK || !sound) return false;

    FMOD::DSP* meter = nullptr;
    FMOD::DSP* fft = nullptr;
    FMOD::Channel* channel = nullptr;
    result = system->createDSPByType(FMOD_DSP_TYPE_LOUDNESS_METER, &meter);
    if (result == FMOD_OK && meter)
    {
        // The loudness DSP also exposes per-channel RMS values. Metering is
        // enabled only on the private no-sound analysis system, never on the
        // live programme mix.
        (void)meter->setMeteringEnabled(false, true);
        result = system->playSound(sound, nullptr, true, &channel);
    }
    if (result == FMOD_OK && channel)
    {
        result = channel->addDSP(0, meter);
    }
    if (result == FMOD_OK &&
        system->createDSPByType(FMOD_DSP_TYPE_FFT, &fft) == FMOD_OK && fft)
    {
        fft->setParameterInt(FMOD_DSP_FFT_WINDOWSIZE, 2048);
        fft->setParameterInt(FMOD_DSP_FFT_WINDOWTYPE, FMOD_DSP_FFT_WINDOW_HANNING);
        if (channel->addDSP(1, fft) != FMOD_OK)
        {
            fft->release();
            fft = nullptr;
        }
    }
    if (result == FMOD_OK)
    {
        meter->setActive(true);
        channel->setPaused(false);
    }

    unsigned int lengthMs = 0;
    sound->getLength(&lengthMs, FMOD_TIMEUNIT_MS);
    int sampleRate = 48000;
    (void)system->getSoftwareFormat(&sampleRate, nullptr, nullptr);
    const std::uint64_t maxUpdates = std::max<std::uint64_t>(
        1000, static_cast<std::uint64_t>(lengthMs / 1000 + 1) * 200);

    bool isPlaying = result == FMOD_OK;
    std::uint64_t updates = 0;
    constexpr unsigned int AnalysisIntervalMs = 2000;
    unsigned int nextAnalysisMs = 0;
    float previousEnergy = 0.0f;
    float observedMaximumTruePeakDb = -80.0f;
    std::vector<float> previousSpectrum;
    std::vector<MusicAnalysis::Frame> frames;
    frames.reserve(lengthMs / AnalysisIntervalMs + 1u);
    while (isPlaying && !stopRequested.load() && updates++ < maxUpdates)
    {
        if (system->update() != FMOD_OK || channel->isPlaying(&isPlaying) != FMOD_OK)
        {
            isPlaying = false;
            result = FMOD_ERR_INTERNAL;
        }
        if (!isPlaying) break;

        unsigned int positionMs = 0;
        if (channel->getPosition(&positionMs, FMOD_TIMEUNIT_MS) != FMOD_OK ||
            positionMs < nextAnalysisMs)
            continue;

        MusicAnalysis::Frame frame;
        // NRT update steps are normally small but are not guaranteed to land
        // exactly on the requested cadence. Label the frame with the actual
        // decoder position so candidates never drift away from the audio.
        frame.timeSeconds = positionMs / 1000.0f;
        void* loudnessData = nullptr;
        unsigned int loudnessDataLength = 0;
        if (meter->getParameterData(
                FMOD_DSP_LOUDNESS_METER_INFO,
                &loudnessData, &loudnessDataLength, nullptr, 0) == FMOD_OK &&
            loudnessData &&
            loudnessDataLength >= sizeof(FMOD_DSP_LOUDNESS_METER_INFO_TYPE))
        {
            const auto* info =
                static_cast<const FMOD_DSP_LOUDNESS_METER_INFO_TYPE*>(loudnessData);
            frame.loudnessLufs = std::isfinite(info->shorttermloudness)
                ? info->shorttermloudness
                : (std::isfinite(info->momentaryloudness)
                       ? info->momentaryloudness
                       : -80.0f);
            frame.truePeakDb = std::isfinite(info->maxtruepeak)
                ? info->maxtruepeak
                : -80.0f;
            observedMaximumTruePeakDb = std::max(
                observedMaximumTruePeakDb, frame.truePeakDb);
            (void)meter->setParameterInt(
                FMOD_DSP_LOUDNESS_METER_STATE,
                FMOD_DSP_LOUDNESS_METER_STATE_RESET_MAXPEAK);
        }

        FMOD_DSP_METERING_INFO metering{};
        if (meter->getMeteringInfo(nullptr, &metering) == FMOD_OK &&
            metering.numchannels > 0)
        {
            const int channelCount = std::min<int>(metering.numchannels, 32);
            double meanSquare = 0.0;
            for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex)
            {
                const double rms = std::clamp(
                    static_cast<double>(metering.rmslevel[channelIndex]),
                    0.0, 1.0);
                meanSquare += rms * rms;
            }
            frame.rms = static_cast<float>(std::sqrt(
                meanSquare / static_cast<double>(channelCount)));
        }

        frame.loudnessLufs = std::clamp(frame.loudnessLufs, -80.0f, 0.0f);
        frame.energy = std::clamp(
            (frame.loudnessLufs + 60.0f) / 52.0f, 0.0f, 1.0f);
        frame.silenceProbability = std::clamp(
            (-45.0f - frame.loudnessLufs) / 20.0f, 0.0f, 1.0f);

        if (fft)
        {
            void* spectrumData = nullptr;
            unsigned int spectrumDataLength = 0;
            if (fft->getParameterData(
                    FMOD_DSP_FFT_SPECTRUMDATA,
                    &spectrumData, &spectrumDataLength, nullptr, 0) == FMOD_OK &&
                spectrumData &&
                spectrumDataLength >= sizeof(FMOD_DSP_PARAMETER_FFT))
            {
                const auto* spectrum =
                    static_cast<const FMOD_DSP_PARAMETER_FFT*>(spectrumData);
                if (spectrum->length > 1 && spectrum->numchannels > 0)
                {
                    std::vector<float> currentSpectrum(
                        static_cast<std::size_t>(spectrum->length), 0.0f);
                    const int channelCount = std::min(spectrum->numchannels, 32);
                    for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex)
                    {
                        if (!spectrum->spectrum[channelIndex]) continue;
                        for (int bin = 0; bin < spectrum->length; ++bin)
                        {
                            currentSpectrum[static_cast<std::size_t>(bin)] +=
                                spectrum->spectrum[channelIndex][bin] /
                                static_cast<float>(channelCount);
                        }
                    }

                    double magnitudeSum = 0.0;
                    double weightedFrequency = 0.0;
                    double positiveFlux = 0.0;
                    const double binFrequency =
                        (sampleRate * 0.5) / static_cast<double>(spectrum->length - 1);
                    for (std::size_t bin = 0; bin < currentSpectrum.size(); ++bin)
                    {
                        const double magnitude = std::max(currentSpectrum[bin], 0.0f);
                        magnitudeSum += magnitude;
                        weightedFrequency += magnitude * binFrequency * bin;
                        if (bin < previousSpectrum.size())
                        {
                            positiveFlux += std::max(
                                static_cast<double>(currentSpectrum[bin] -
                                                    previousSpectrum[bin]),
                                0.0);
                        }
                    }
                    if (magnitudeSum > 0.0)
                    {
                        frame.spectralCentroidHz = static_cast<float>(
                            weightedFrequency / magnitudeSum);
                        frame.spectralFlux = static_cast<float>(std::clamp(
                            positiveFlux / magnitudeSum, 0.0, 1.0));
                    }
                    previousSpectrum = std::move(currentSpectrum);
                }
            }
        }

        frame.energySlope = std::clamp(
            frame.energy - previousEnergy, -1.0f, 1.0f);
        frame.onsetStrength = std::clamp(
            0.7f * frame.spectralFlux +
                0.3f * std::max(frame.energySlope, 0.0f),
            0.0f, 1.0f);
        previousEnergy = frame.energy;
        frames.push_back(frame);
        nextAnalysisMs = positionMs + AnalysisIntervalMs;
    }

    const bool analysisCompleted =
        !stopRequested.load() && result == FMOD_OK && updates < maxUpdates;
    bool success = false;
    if (analysisCompleted)
    {
        void* data = nullptr;
        unsigned int dataLength = 0;
        if (meter->getParameterData(
                FMOD_DSP_LOUDNESS_METER_INFO, &data, &dataLength, nullptr, 0) == FMOD_OK &&
            data && dataLength >= sizeof(FMOD_DSP_LOUDNESS_METER_INFO_TYPE))
        {
            const auto* info = static_cast<const FMOD_DSP_LOUDNESS_METER_INFO_TYPE*>(data);
            integratedLufs = info->integratedloudness;
            truePeakDb = std::max(info->maxtruepeak, observedMaximumTruePeakDb);
            success = std::isfinite(integratedLufs) && integratedLufs > -80.0f &&
                      std::isfinite(truePeakDb);
        }
    }

    // A very short asset can complete before the loudness meter has accumulated
    // a valid integrated LUFS value. Its local frames are still useful for
    // segment selection, provided decoding itself completed successfully.
    if (analysisCompleted && !frames.empty())
    {
        musicAnalysis = MusicAnalysis::AnalyzeTrack(
            lengthMs / 1000.0f, std::move(frames));
    }

    if (channel)
    {
        channel->stop();
        if (meter) channel->removeDSP(meter);
        if (fft) channel->removeDSP(fft);
    }
    if (fft) fft->release();
    if (meter) meter->release();
    sound->release();
    system->update();
    return success;
}
}

AudioManager::~AudioManager()
{
    try
    {
        if (m_playbackMemoryStoreAvailable)
        {
            std::string ignored;
            (void)FlushPlaybackMemory(ignored);
        }
        else if (m_activePlaybackMemoryFlushMetadata)
        {
            bool completed = false;
            std::string ignored;
            (void)CompletePlaybackMemoryFlush(true, completed, ignored);
        }
    }
    catch (...)
    {
        // Destructors must never propagate. Normal application shutdown calls
        // Shutdown(), which reports any persistence error before this fallback.
    }
}

bool AudioManager::LoadSound(
    const std::string& soundName, const std::string& filePath, bool isStream, SoundKind kind)
{
    return LoadSoundInternal(soundName, filePath, isStream, kind, false);
}

bool AudioManager::LoadSoundInternal(
    const std::string& soundName, const std::string& filePath, bool isStream,
    SoundKind kind, bool replaceExisting)
{
    auto existing = m_sounds.find(soundName);
    if (existing != m_sounds.end())
    {
        if (existing->second.filePath == filePath && existing->second.kind == kind)
        {
            spdlog::debug("Sound '{}' is already loaded from the requested path.", soundName);
            return true;
        }
        if (!replaceExisting)
        {
            spdlog::error(
                "Sound '{}' is already loaded as '{}' from '{}' and cannot be replaced as '{}' from '{}'.",
                soundName, SoundKindToString(existing->second.kind), existing->second.filePath,
                SoundKindToString(kind), filePath);
            return false;
        }
    }

    FMOD_MODE mode = FMOD_DEFAULT;
    if (isStream)
    {
        mode |= FMOD_CREATESTREAM;
    }

    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system)
    {
        spdlog::error("Cannot load sound '{}': FMOD system is not initialized.", soundName);
        return false;
    }

    FMOD::Sound* newSound = nullptr;
    FMOD_RESULT result = system->createSound(filePath.c_str(), mode, nullptr, &newSound);
    if (result != FMOD_OK)
    {
        spdlog::error("FMOD createSound failed: {} for file: {}", FMOD_ErrorString(result), filePath);
        return false;
    }
    else
    {
        spdlog::info("FMOD createSound success: {} for file: {}", soundName, filePath);
    }
    
    SoundData data;
    data.sound = newSound;
    data.filePath = filePath;
    data.kind = kind;
    data.isMusic = kind == SoundKind::Music;
    if (data.isMusic)
    {
        unsigned int durationMs = 0;
        if (newSound->getLength(&durationMs, FMOD_TIMEUNIT_MS) == FMOD_OK &&
            durationMs > 0)
        {
            MusicAnalysis::FileIdentity identity;
            std::string identityError;
            if (MusicAnalysis::ReadFileIdentity(
                    PathFromUtf8(filePath), identity, identityError))
            {
                for (const auto& [loadedName, loaded] : m_sounds)
                {
                    (void)loadedName;
                    if (loaded.listeningFatigue && SameFileIdentity(
                            loaded.listeningFatigue->identity, identity))
                    {
                        data.listeningFatigue = loaded.listeningFatigue;
                        break;
                    }
                }
                if (!data.listeningFatigue)
                {
                    for (const auto& retained : m_retainedPlaybackMemory)
                    {
                        if (retained && SameFileIdentity(
                                retained->identity, identity))
                        {
                            data.listeningFatigue = retained;
                            break;
                        }
                    }
                }
            }
            else
            {
                MarkPlaybackMemoryFailure(identityError);
                spdlog::warn(
                    "Unable to identify '{}' for playback memory: {}",
                    filePath, identityError);
            }

            if (!data.listeningFatigue)
            {
                data.listeningFatigue =
                    std::make_shared<ListeningFatigueData>();
                data.listeningFatigue->identity = std::move(identity);
                data.listeningFatigue->state = ListeningHeatmap::CreateState(
                    durationMs / 1000.0);
                LoadPlaybackMemoryForSound(data);
            }
        }
    }

    // Commit a replacement only after FMOD has opened the new resource. A bad
    // path therefore leaves the previous asset available.
    if (existing != m_sounds.end())
    {
        PruneStoppedChannels(existing->second);
        for (auto* channel : existing->second.channels)
        {
            if (!IsChannelPlayingSound(channel, existing->second.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
        }
        existing->second.channels.clear();

        if (existing->second.sound)
        {
            const FMOD_RESULT releaseResult = existing->second.sound->release();
            if (releaseResult != FMOD_OK)
            {
                spdlog::error(
                    "Failed to release previous sound '{}': {}. Replacement aborted.",
                    soundName, FMOD_ErrorString(releaseResult));
                newSound->release();
                return false;
            }
        }
        RetainPlaybackMemoryState(existing->second.listeningFatigue);
        existing->second = std::move(data);
        RegisterPlaybackMemoryState(existing->second.listeningFatigue);
    }
    else
    {
        const auto [inserted, wasInserted] =
            m_sounds.emplace(soundName, std::move(data));
        (void)wasInserted;
        RegisterPlaybackMemoryState(inserted->second.listeningFatigue);
    }

    spdlog::info("Sound loaded successfully: {}", soundName);
    return true;
}

bool AudioManager::UnloadSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it != m_sounds.end())
    {
        PruneStoppedChannels(it->second);
        for (auto* channel : it->second.channels)
        {
            if (!IsChannelPlayingSound(channel, it->second.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
        }
        it->second.channels.clear();

        if (it->second.sound)
        {
            FMOD_RESULT result = it->second.sound->release();
            if (result != FMOD_OK)
            {
                spdlog::error("Failed to release sound {}: {}", soundName, FMOD_ErrorString(result));
                return false;
            }
            it->second.sound = nullptr;
        }

        RetainPlaybackMemoryState(it->second.listeningFatigue);
        m_sounds.erase(it);
        PruneRetainedPlaybackMemory();
        spdlog::info("Sound {} unloaded successfully", soundName);
        return true;
    }
    
    spdlog::warn("Attempted to unload non-existent sound: {}", soundName);
    return false;
}

FMOD::Channel* AudioManager::PlaySound(const std::string& soundName, bool loop, float volume, float pitch)
{
    return PlaySoundInternal(soundName, loop, volume, pitch, false);
}

FMOD::Channel* AudioManager::PlayMusic(const std::string& soundName, bool loop, float volume, float pitch)
{
    return PlaySoundInternal(soundName, loop, volume, pitch, true);
}

FMOD::Channel* AudioManager::PlaySoundInternal(
    const std::string& soundName, bool loop, float volume, float pitch, bool normalizeMusic)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
    {
        spdlog::error("Sound not found: {}", soundName);
        return nullptr;
    }
    
    SoundData& data = it->second;
    if (!data.sound)
    {
        spdlog::error("Sound not valid: {}", soundName);
        return nullptr;
    }

    if (m_normalPlaybackBlocked && data.kind != SoundKind::Emergency)
    {
        spdlog::warn(
            "Playback of '{}' was blocked by the cinema safety gate.", soundName);
        return nullptr;
    }
    
    FMOD_MODE currentMode = FMOD_DEFAULT;
    FMOD_RESULT modeResult = data.sound->getMode(&currentMode);
    if (modeResult != FMOD_OK)
    {
        spdlog::error("FMOD getMode failed for '{}': {}", soundName, FMOD_ErrorString(modeResult));
        return nullptr;
    }

    if (normalizeMusic)
    {
        if (data.kind != SoundKind::Music)
        {
            spdlog::error(
                "Sound '{}' is classified as '{}' and cannot be played as music.",
                soundName, SoundKindToString(data.kind));
            return nullptr;
        }
        data.isMusic = true;
        QueueLoudnessAnalysis(soundName);
    }
    if (loop)
        currentMode |= FMOD_LOOP_NORMAL;
    else
        currentMode &= ~FMOD_LOOP_NORMAL;
    data.sound->setMode(currentMode);

    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system)
    {
        spdlog::error("Cannot play sound '{}': FMOD system is not initialized.", soundName);
        return nullptr;
    }

    FMOD::ChannelGroup* targetGroup = nullptr;
    if (data.kind == SoundKind::Music)
    {
        if (!EnsureMusicProcessing()) return nullptr;
        targetGroup = m_musicChannelGroup;
    }
    else if (data.kind == SoundKind::Emergency)
    {
        if (!EnsureChannelGroup(m_emergencyChannelGroup, "TSM Emergency"))
            return nullptr;
        targetGroup = m_emergencyChannelGroup;
    }
    else if (data.kind == SoundKind::Announcement)
    {
        if (!EnsureChannelGroup(m_announcementChannelGroup, "TSM Announcements"))
            return nullptr;
        targetGroup = m_announcementChannelGroup;
    }
    else
    {
        if (!EnsureChannelGroup(m_sfxChannelGroup, "TSM SFX")) return nullptr;
        targetGroup = m_sfxChannelGroup;
    }

    FMOD::Channel* channel = nullptr;
    FMOD_RESULT result = system->playSound(data.sound, targetGroup, true, &channel);
    if (result != FMOD_OK || !channel)
    {
        spdlog::error("FMOD playSound failed: {}", FMOD_ErrorString(result));
        return nullptr;
    }
    else
    {
        spdlog::info("FMOD playSound success: {}", soundName);
    }
    
    const float normalizedVolume = normalizeMusic
        ? volume * data.normalizationGainLinear
        : volume;
    channel->setVolume(normalizedVolume);
    ApplyPitch(channel, pitch);
    channel->setPaused(false);
    CancelChannelFade(channel);
    RefreshChannelState();
    MixerState::GetInstance().ApplyAllVolumes();

    float volumeTemp = 0.0f;
    if (channel->getVolume(&volumeTemp) == FMOD_OK)
    {
        spdlog::info("Volume of {} after configuration: {}", soundName, volumeTemp);
    }

    data.channels.erase(
        std::remove(data.channels.begin(), data.channels.end(), channel),
        data.channels.end());
    data.channels.push_back(channel);

    return channel;
}

void AudioManager::StopSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    PruneStoppedChannels(it->second);
    for (auto* channel : it->second.channels)
    {
        if (!IsChannelPlayingSound(channel, it->second.sound)) continue;
        CancelChannelFade(channel);
        channel->stop();
    }

    it->second.channels.clear();
}

void AudioManager::StopAllSounds()
{
    for (auto& pair : m_sounds)
    {
        SoundData& data = pair.second;
        PruneStoppedChannels(data);
        for (auto* channel : data.channels)
        {
            if (!IsChannelPlayingSound(channel, data.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
        }
        data.channels.clear();
    }
}

void AudioManager::StopAllNonEmergencyImmediately()
{
    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (data.kind == SoundKind::Emergency) continue;
        PruneStoppedChannels(data);
        for (FMOD::Channel* channel : data.channels)
        {
            if (!IsChannelPlayingSound(channel, data.sound)) continue;
            CancelChannelFade(channel);
            channel->stop();
        }
        data.channels.clear();
    }

    // Channel groups are the final containment boundary, including voices no
    // longer represented in a logical SoundData channel list.
    if (m_musicChannelGroup) m_musicChannelGroup->stop();
    if (m_announcementChannelGroup) m_announcementChannelGroup->stop();
    if (m_sfxChannelGroup) m_sfxChannelGroup->stop();
}

void AudioManager::SetVolume(const std::string& soundName, float volume)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    PruneStoppedChannels(it->second);
    for (auto* channel : it->second.channels)
    {
        if (IsChannelPlayingSound(channel, it->second.sound)) channel->setVolume(volume);
    }
}

void AudioManager::SetPitch(const std::string& soundName, float pitch)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return;

    PruneStoppedChannels(it->second);
    for (auto* channel : it->second.channels)
    {
        if (IsChannelPlayingSound(channel, it->second.sound)) ApplyPitch(channel, pitch);
    }
}

void AudioManager::SetChannelVolume(FMOD::Channel* channel, float volume)
{
    if (channel)
    {
        // An explicit envelope owner such as a playlist crossfade must not
        // race a background normalization or stop fade.
        CancelChannelFade(channel);
        channel->setVolume(volume);
    }
}

void AudioManager::ReconcileMusicChannelVolume(FMOD::Channel* channel)
{
    if (!channel) return;
    RetargetChannelFade(channel, GetNormalizationGainForChannel(channel));
}

void AudioManager::SetChannelPitch(FMOD::Channel* channel, float pitch)
{
    ApplyPitch(channel, pitch);
}

void AudioManager::ApplyMixerVolumes(
    float master, float music, float announcement, float sfx, float effectiveDuck)
{
    const float safeMaster = std::max(master, 0.0f);
    const float safetyFactor = m_normalPlaybackBlocked ? 0.0f : 1.0f;
    if (m_musicChannelGroup)
        m_musicChannelGroup->setVolume(safetyFactor * safeMaster * std::max(music, 0.0f) *
                                       std::max(effectiveDuck, 0.0f));
    if (m_announcementChannelGroup)
        m_announcementChannelGroup->setVolume(
            safetyFactor * safeMaster * std::max(announcement, 0.0f));
    if (m_sfxChannelGroup)
        m_sfxChannelGroup->setVolume(safetyFactor * safeMaster * std::max(sfx, 0.0f));
    if (m_emergencyChannelGroup) m_emergencyChannelGroup->setVolume(1.0f);
}

void AudioManager::SetNormalPlaybackBlocked(bool blocked)
{
    m_normalPlaybackBlocked = blocked;
    MixerState::GetInstance().ApplyAllVolumes();
}
void AudioManager::Update(float deltaTime)
{
    ApplyLoudnessResults();

    bool flushCompleted = false;
    std::string flushError;
    if (!CompletePlaybackMemoryFlush(
            false, flushCompleted, flushError) && flushCompleted)
    {
        spdlog::warn("Unable to persist playback memory: {}", flushError);
    }

    m_playbackMemoryFlushTimer += std::max(deltaTime, 0.0f);
    if (m_playbackMemoryStoreAvailable &&
        !m_activePlaybackMemoryFlushMetadata &&
        m_playbackMemoryFlushTimer >= 5.0f)
    {
        flushError.clear();
        if (!StartPlaybackMemoryFlush(flushError))
        {
            spdlog::warn("Unable to persist playback memory: {}", flushError);
            // Back off before retrying so a damaged disk cannot flood logs or
            // starve the real-time control loop.
            m_playbackMemoryFlushTimer = 0.0f;
        }
    }

    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (system)
    {
        system->update();
    }

    for (auto fadeIt = m_channelFades.begin(); fadeIt != m_channelFades.end();)
    {
        ChannelFade& fade = *fadeIt;
        bool isPlaying = false;
        FMOD::Sound* currentSound = nullptr;
        const bool valid = fade.channel &&
            fade.channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
            fade.channel->getCurrentSound(&currentSound) == FMOD_OK &&
            currentSound == fade.expectedSound;

        if (!valid)
        {
            fadeIt = m_channelFades.erase(fadeIt);
            continue;
        }

        fade.timer += deltaTime;
        const float progress = fade.duration <= 0.0f
            ? 1.0f
            : std::min(fade.timer / fade.duration, 1.0f);
        const float volume = fade.startVolume + (fade.targetVolume - fade.startVolume) * progress;
        fade.channel->setVolume(volume);

        if (progress >= 1.0f)
        {
            if (fade.stopWhenComplete)
            {
                fade.channel->stop();
            }
            fadeIt = m_channelFades.erase(fadeIt);
        }
        else
        {
            ++fadeIt;
        }
    }

    RefreshChannelState();
}

void AudioManager::RefreshChannelState()
{
    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        PruneStoppedChannels(data);
    }
}

FMOD::Channel* AudioManager::GetLastChannelOfSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
        return nullptr;

    PruneStoppedChannels(it->second);
    if (it->second.channels.empty())
        return nullptr;

    return it->second.channels.back();
}

bool AudioManager::LoadAnnouncement(const std::string& announcementId, const std::string& filePath)
{
    StopSound(announcementId);
    if (m_sounds.find(announcementId) != m_sounds.end()) {
        UnloadSound(announcementId);
    }
    
    bool success = LoadSound(announcementId, filePath, true, SoundKind::Announcement);
    
    if (success) {
        spdlog::info("Announcement '{}' loaded successfully: {}", announcementId, filePath);
    } else {
        spdlog::error("Failed to load announcement '{}': {}", announcementId, filePath);
    }
    
    return success;
}

bool AudioManager::LoadEmergencySound(
    const std::string& soundName, const std::string& filePath)
{
    const bool success = LoadSoundInternal(
        soundName, filePath, true, SoundKind::Emergency, true);
    if (success)
        spdlog::info("Emergency preset '{}' loaded successfully: {}", soundName, filePath);
    else
        spdlog::error("Failed to load emergency preset '{}': {}", soundName, filePath);
    return success;
}

FMOD::Sound* AudioManager::GetSound(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it != m_sounds.end()) {
        return it->second.sound;
    }
    return nullptr;
}

FMOD::Channel* AudioManager::PlaySoundWithFadeIn(const std::string& soundName, bool loop, float volume, float pitch)
{
    FMOD::Channel* channel = PlaySoundInternal(soundName, loop, 0.0f, pitch, false);
    if (!channel)
        return nullptr;

    StartChannelFade(channel, volume, false);
    spdlog::info("Starting fade-in for sound: {} (target volume: {})", soundName, volume);
    return channel;
}

FMOD::Channel* AudioManager::PlayMusicWithFadeIn(const std::string& soundName, bool loop, float volume, float pitch)
{
    FMOD::Channel* channel = PlaySoundInternal(soundName, loop, 0.0f, pitch, true);
    if (!channel) return nullptr;

    float normalizationGain = 1.0f;
    const auto soundIt = m_sounds.find(soundName);
    if (soundIt != m_sounds.end()) normalizationGain = soundIt->second.normalizationGainLinear;
    StartChannelFade(channel, volume * normalizationGain, false);
    spdlog::info("Starting normalized music fade-in: {} (target volume: {})", soundName, volume);
    return channel;
}

void AudioManager::QueueLoudnessAnalysis(const std::string& soundName)
{
    auto soundIt = m_sounds.find(soundName);
    if (soundIt == m_sounds.end() || soundIt->second.filePath.empty()) return;

    SoundData& data = soundIt->second;
    if (data.kind != SoundKind::Music) return;
    data.isMusic = true;
    if (data.loudnessStatus == LoudnessStatus::Queued ||
        data.loudnessStatus == LoudnessStatus::Analyzing ||
        data.loudnessStatus == LoudnessStatus::Ready)
    {
        return;
    }

    data.loudnessStatus = LoudnessStatus::Queued;
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        m_loudnessTasks.push_back({soundName, data.filePath});
    }
    StartLoudnessWorker();
    m_loudnessCondition.notify_one();
}

void AudioManager::StartLoudnessWorker()
{
    if (m_loudnessThreadStarted && !m_loudnessWorkerExited.load()) return;
    if (m_loudnessThread.joinable()) m_loudnessThread.join();
    m_loudnessThreadStarted = true;
    m_loudnessWorkerExited.store(false);
    m_stopLoudnessThread.store(false);
    m_loudnessThread = std::thread(&AudioManager::LoudnessWorkerMain, this);
}

void AudioManager::LoudnessWorkerMain()
{
    FMOD::System* analysisSystem = nullptr;
    LoudnessTask activeTask;
    bool hasActiveTask = false;

    const auto failPendingTasks = [this, &activeTask, &hasActiveTask]
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        if (hasActiveTask)
        {
            m_loudnessResults.push_back(
                {activeTask.soundName, activeTask.filePath, false, 0.0f, 0.0f});
            hasActiveTask = false;
        }
        while (!m_loudnessTasks.empty())
        {
            LoudnessTask task = std::move(m_loudnessTasks.front());
            m_loudnessTasks.pop_front();
            m_loudnessResults.push_back(
                {task.soundName, task.filePath, false, 0.0f, 0.0f});
        }
        m_activeLoudnessSound.clear();
    };

    try
    {
        nlohmann::json cache = LoadLoudnessCache(m_loudnessCachePath);
        MusicAnalysis::AnalysisStore analysisStore(
            PathFromUtf8(m_musicAnalysisDatabasePath));
        std::string analysisStoreError;
        const bool analysisStoreAvailable =
            analysisStore.Initialize(analysisStoreError);
        if (!analysisStoreAvailable)
        {
            spdlog::warn(
                "Smart music analysis cache unavailable '{}': {}",
                m_musicAnalysisDatabasePath, analysisStoreError);
        }

        FMOD_RESULT result = FMOD::System_Create(&analysisSystem);
        if (result == FMOD_OK && analysisSystem)
        {
            result = analysisSystem->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT);
        }
        if (result == FMOD_OK && analysisSystem)
        {
            result = analysisSystem->init(8, FMOD_INIT_STREAM_FROM_UPDATE, nullptr);
        }
        if (result != FMOD_OK || !analysisSystem)
        {
            spdlog::error(
                "Unable to initialize LUFS analysis engine: {}", FMOD_ErrorString(result));
            failPendingTasks();
        }
        else
        {
            while (!m_stopLoudnessThread.load())
            {
                {
                    std::unique_lock<std::mutex> lock(m_loudnessMutex);
                    m_loudnessCondition.wait(lock, [this]
                    {
                        return m_stopLoudnessThread.load() || !m_loudnessTasks.empty();
                    });
                    if (m_stopLoudnessThread.load()) break;
                    activeTask = std::move(m_loudnessTasks.front());
                    m_loudnessTasks.pop_front();
                    hasActiveTask = true;
                    m_activeLoudnessSound = activeTask.soundName;
                }

                LoudnessResult loudnessResult;
                loudnessResult.soundName = activeTask.soundName;
                loudnessResult.filePath = activeTask.filePath;
                try
                {
                    const std::string cacheKey = NormalizeCacheKey(activeTask.filePath);
                    const std::uintmax_t fileSize = GetFileSize(activeTask.filePath);
                    const std::int64_t writeTime = GetFileWriteTime(activeTask.filePath);
                    const auto cacheIt = cache.find(cacheKey);
                    LoudnessCacheEntry cacheEntry;
                    const bool loudnessCacheHit = cacheIt != cache.end() &&
                        ReadLoudnessCacheEntry(*cacheIt, cacheEntry) &&
                        cacheEntry.size == fileSize && cacheEntry.writeTime == writeTime;

                    MusicAnalysis::FileIdentity identity;
                    std::string identityError;
                    const bool hasIdentity = MusicAnalysis::ReadFileIdentity(
                        PathFromUtf8(activeTask.filePath), identity, identityError);
                    MusicAnalysis::StoreLookupStatus analysisStatus =
                        MusicAnalysis::StoreLookupStatus::Miss;
                    if (analysisStoreAvailable && hasIdentity)
                    {
                        std::string loadError;
                        analysisStatus = analysisStore.Load(
                            identity, loudnessResult.musicAnalysis, loadError);
                        if (analysisStatus ==
                            MusicAnalysis::StoreLookupStatus::Unavailable)
                        {
                            spdlog::warn(
                                "Unable to read smart analysis for '{}': {}",
                                activeTask.soundName, loadError);
                        }
                    }

                    if (loudnessCacheHit &&
                        analysisStatus == MusicAnalysis::StoreLookupStatus::Hit)
                    {
                        loudnessResult.integratedLufs = cacheEntry.integratedLufs;
                        loudnessResult.truePeakDb = cacheEntry.truePeakDb;
                        loudnessResult.success = true;
                        loudnessResult.musicAnalysisSuccess = true;
                    }
                    else
                    {
                        float analyzedLufs = 0.0f;
                        float analyzedTruePeakDb = 0.0f;
                        const bool loudnessAnalysisSucceeded = AnalyzeTrackAudio(
                            analysisSystem, activeTask.filePath, m_stopLoudnessThread,
                            analyzedLufs, analyzedTruePeakDb,
                            loudnessResult.musicAnalysis);
                        loudnessResult.musicAnalysisSuccess =
                            !loudnessResult.musicAnalysis.frames.empty();

                        // The legacy loudness cache and the richer analysis
                        // cache are independent. A valid LUFS cache remains
                        // authoritative even when the optional smart analysis
                        // is missing or a very short asset cannot yield an
                        // integrated meter value.
                        if (loudnessCacheHit)
                        {
                            loudnessResult.integratedLufs = cacheEntry.integratedLufs;
                            loudnessResult.truePeakDb = cacheEntry.truePeakDb;
                            loudnessResult.success = true;
                        }
                        else
                        {
                            loudnessResult.integratedLufs = analyzedLufs;
                            loudnessResult.truePeakDb = analyzedTruePeakDb;
                            loudnessResult.success = loudnessAnalysisSucceeded;
                        }

                        if (loudnessAnalysisSucceeded && !loudnessCacheHit)
                        {
                            cache[cacheKey] = {
                                {"size", fileSize},
                                {"writeTime", writeTime},
                                {"integratedLufs", loudnessResult.integratedLufs},
                                {"truePeakDb", loudnessResult.truePeakDb}
                            };
                            std::string saveError;
                            if (!SaveLoudnessCacheAtomically(
                                    m_loudnessCachePath, cache, saveError))
                            {
                                spdlog::warn(
                                    "Unable to save loudness cache '{}': {}",
                                    m_loudnessCachePath, saveError);
                            }
                        }
                        if (analysisStoreAvailable && hasIdentity &&
                            loudnessResult.musicAnalysisSuccess)
                        {
                            std::string saveAnalysisError;
                            if (!analysisStore.Save(
                                    identity, loudnessResult.musicAnalysis,
                                    saveAnalysisError))
                            {
                                spdlog::warn(
                                    "Unable to cache smart analysis for '{}': {}",
                                    activeTask.soundName, saveAnalysisError);
                            }
                        }
                    }
                }
                catch (const std::exception& error)
                {
                    loudnessResult.success = false;
                    spdlog::error(
                        "Unexpected LUFS analysis error for '{}': {}",
                        activeTask.soundName, error.what());
                }
                catch (...)
                {
                    loudnessResult.success = false;
                    spdlog::error(
                        "Unknown LUFS analysis error for '{}'.", activeTask.soundName);
                }

                {
                    std::lock_guard<std::mutex> lock(m_loudnessMutex);
                    m_activeLoudnessSound.clear();
                    hasActiveTask = false;
                    m_loudnessResults.push_back(std::move(loudnessResult));
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        spdlog::error("LUFS worker stopped after an unexpected error: {}", error.what());
        failPendingTasks();
    }
    catch (...)
    {
        spdlog::error("LUFS worker stopped after an unknown error.");
        failPendingTasks();
    }

    if (analysisSystem)
    {
        analysisSystem->close();
        analysisSystem->release();
    }
    m_loudnessWorkerExited.store(true);
}

void AudioManager::ApplyLoudnessResults()
{
    std::deque<LoudnessResult> results;
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        results.swap(m_loudnessResults);
    }

    for (const LoudnessResult& result : results)
    {
        auto soundIt = m_sounds.find(result.soundName);
        if (soundIt == m_sounds.end() || soundIt->second.filePath != result.filePath) continue;

        SoundData& data = soundIt->second;
        if (!result.success)
        {
            data.loudnessStatus = LoudnessStatus::Failed;
            spdlog::warn("LUFS analysis failed for '{}'", result.soundName);
            continue;
        }

        const float oldGain = data.normalizationGainLinear;
        float gainDb = std::clamp(m_loudnessTargetLufs - result.integratedLufs, -12.0f, 12.0f);
        if (result.truePeakDb + gainDb > -1.0f)
        {
            gainDb = -1.0f - result.truePeakDb;
        }

        data.integratedLufs = result.integratedLufs;
        data.truePeakDb = result.truePeakDb;
        data.normalizationGainDb = gainDb;
        data.normalizationGainLinear = std::pow(10.0f, gainDb / 20.0f);
        data.loudnessStatus = LoudnessStatus::Ready;
        data.musicAnalysisReady = result.musicAnalysisSuccess;
        data.musicAnalysis = result.musicAnalysisSuccess
            ? result.musicAnalysis
            : MusicAnalysis::TrackAnalysis{};

        const float gainRatio = oldGain > 0.0f ? data.normalizationGainLinear / oldGain : 1.0f;
        PruneStoppedChannels(data);
        for (FMOD::Channel* channel : data.channels)
        {
            bool isPlaying = false;
            float currentVolume = 0.0f;
            if (channel && channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
                channel->getVolume(&currentVolume) == FMOD_OK)
            {
                AdjustChannelNormalizationGain(channel, gainRatio);
            }
        }

        spdlog::info("LUFS '{}' = {:.2f}, true peak {:.2f} dB, gain {:+.2f} dB",
                     result.soundName, result.integratedLufs, result.truePeakDb, gainDb);
    }
}

float AudioManager::GetNormalizationGainForChannel(FMOD::Channel* channel) const
{
    if (!channel) return 1.0f;
    FMOD::Sound* sound = nullptr;
    if (channel->getCurrentSound(&sound) != FMOD_OK || !sound) return 1.0f;

    for (const auto& [name, data] : m_sounds)
    {
        (void)name;
        if (data.sound == sound && data.isMusic) return data.normalizationGainLinear;
    }
    return 1.0f;
}

std::vector<AudioManager::LoudnessDiagnostic> AudioManager::GetLoudnessDiagnostics() const
{
    std::string activeSound;
    std::map<std::string, int> queuePositions;
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        activeSound = m_activeLoudnessSound;
        int position = 1;
        for (const LoudnessTask& task : m_loudnessTasks)
        {
            queuePositions.emplace(task.soundName, position++);
        }
    }

    std::vector<LoudnessDiagnostic> diagnostics;
    for (const auto& [soundName, data] : m_sounds)
    {
        if (!data.isMusic) continue;
        LoudnessStatus status = data.loudnessStatus;
        if (soundName == activeSound) status = LoudnessStatus::Analyzing;
        const auto queueIt = queuePositions.find(soundName);
        const int queuePosition = queueIt != queuePositions.end() ? queueIt->second : -1;
        diagnostics.push_back({
            soundName, data.filePath, status, data.integratedLufs,
            data.truePeakDb, data.normalizationGainDb, queuePosition,
            data.musicAnalysisReady
        });
    }
    const auto statusRank = [](LoudnessStatus status)
    {
        switch (status)
        {
            case LoudnessStatus::Analyzing: return 0;
            case LoudnessStatus::Queued: return 1;
            case LoudnessStatus::Ready: return 2;
            case LoudnessStatus::Failed: return 3;
            case LoudnessStatus::NotQueued: return 4;
        }
        return 5;
    };
    std::sort(diagnostics.begin(), diagnostics.end(), [&](const auto& left, const auto& right)
    {
        const int leftRank = statusRank(left.status);
        const int rightRank = statusRank(right.status);
        if (leftRank != rightRank) return leftRank < rightRank;
        if (left.status == LoudnessStatus::Queued &&
            left.queuePosition != right.queuePosition)
        {
            return left.queuePosition < right.queuePosition;
        }
        return left.filePath < right.filePath;
    });
    return diagnostics;
}

const MusicAnalysis::TrackAnalysis* AudioManager::GetMusicAnalysis(
    const std::string& soundName) const
{
    const auto sound = m_sounds.find(soundName);
    if (sound == m_sounds.end() || !sound->second.musicAnalysisReady) return nullptr;
    return &sound->second.musicAnalysis;
}

const ListeningHeatmap::State* AudioManager::GetListeningFatigue(
    const std::string& soundName) const
{
    const auto sound = m_sounds.find(soundName);
    if (sound == m_sounds.end() || !sound->second.listeningFatigue)
        return nullptr;
    return &sound->second.listeningFatigue->state;
}

void AudioManager::LoadPlaybackMemoryForSound(
    SoundData& data,
    bool resetOnMiss)
{
    if (!data.isMusic || !data.listeningFatigue ||
        data.listeningFatigue->readConclusive ||
        data.listeningFatigue->identity.normalizedPath.empty() ||
        !m_playbackMemoryStoreAvailable || !m_playbackMemoryStore)
        return;
    if (data.listeningFatigue->pathGeneration != 0 &&
        !IsCurrentPlaybackMemoryState(data.listeningFatigue))
        return;

    std::string errorMessage;
    ListeningHeatmap::State restored;
    const PlaybackMemory::StoreLookupStatus status =
        m_playbackMemoryStore->LoadHeatmap(
            data.listeningFatigue->identity, restored, errorMessage);
    if (status == PlaybackMemory::StoreLookupStatus::Hit)
    {
        if (std::abs(
                restored.trackDurationSeconds -
                data.listeningFatigue->state.trackDurationSeconds) <= 0.1)
        {
            if (data.listeningFatigue->hasPendingMutations)
            {
                ListeningHeatmap::State merged;
                if (!MergePlaybackHeatmaps(
                        restored,
                        data.listeningFatigue->pendingMutations,
                        merged))
                {
                    const std::string mismatch =
                        "Playback heatmap settings do not match pending RAM state for '" +
                        data.filePath + "'.";
                    MarkPlaybackMemoryFailure(mismatch);
                    spdlog::warn("{}", mismatch);
                    data.listeningFatigue->state =
                        data.listeningFatigue->pendingMutations;
                    data.listeningFatigue->pendingMutations = {};
                    data.listeningFatigue->hasPendingMutations = false;
                    data.listeningFatigue->readConclusive = true;
                    data.listeningFatigue->dirty = true;
                    BumpPlaybackMemoryVersion(
                        data.listeningFatigue->version);
                    return;
                }
                data.listeningFatigue->state = std::move(merged);
                data.listeningFatigue->dirty = true;
            }
            else
            {
                data.listeningFatigue->state = std::move(restored);
                data.listeningFatigue->dirty = false;
            }
            data.listeningFatigue->pendingMutations = {};
            data.listeningFatigue->hasPendingMutations = false;
            data.listeningFatigue->readConclusive = true;
            BumpPlaybackMemoryVersion(data.listeningFatigue->version);
        }
        else
        {
            const std::string mismatch =
                "Playback heatmap duration does not match '" + data.filePath + "'.";
            if (data.listeningFatigue->hasPendingMutations)
                data.listeningFatigue->state =
                    data.listeningFatigue->pendingMutations;
            else
                data.listeningFatigue->state = ListeningHeatmap::CreateState(
                    data.listeningFatigue->state.trackDurationSeconds,
                    {data.listeningFatigue->state.bucketDurationSeconds,
                     data.listeningFatigue->state.dailyDecayFactor});
            data.listeningFatigue->pendingMutations = {};
            data.listeningFatigue->hasPendingMutations = false;
            data.listeningFatigue->readConclusive = true;
            data.listeningFatigue->dirty = true;
            BumpPlaybackMemoryVersion(data.listeningFatigue->version);
            MarkPlaybackMemoryFailure(mismatch, true);
            spdlog::warn("{}", mismatch);
        }
    }
    else if (status == PlaybackMemory::StoreLookupStatus::Unavailable)
    {
        MarkPlaybackMemoryFailure(errorMessage);
        spdlog::warn(
            "Unable to restore playback heatmap for '{}': {}",
            data.filePath, errorMessage);
    }
    else
    {
        (void)resetOnMiss;
        const bool hadPendingMutations =
            data.listeningFatigue->hasPendingMutations;
        if (hadPendingMutations)
            data.listeningFatigue->state =
                data.listeningFatigue->pendingMutations;
        else
            data.listeningFatigue->state = ListeningHeatmap::CreateState(
                data.listeningFatigue->state.trackDurationSeconds);
        data.listeningFatigue->pendingMutations = {};
        data.listeningFatigue->hasPendingMutations = false;
        data.listeningFatigue->readConclusive = true;
        data.listeningFatigue->dirty = hadPendingMutations;
        BumpPlaybackMemoryVersion(data.listeningFatigue->version);

        // A stale row is a normal cache miss, but persist the new identity even
        // before first playback so it becomes authoritative for this path.
        if (!errorMessage.empty())
        {
            data.listeningFatigue->dirty = true;
        }
    }
}

void AudioManager::ResolveTransitionHistoryRead()
{
    if (m_transitionHistoryReadConclusive ||
        !m_playbackMemoryStoreAvailable || !m_playbackMemoryStore)
        return;

    std::string errorMessage;
    TransitionHistory::History restored;
    const PlaybackMemory::StoreLookupStatus status =
        m_playbackMemoryStore->LoadTransitionHistory(restored, errorMessage);
    if (status == PlaybackMemory::StoreLookupStatus::Unavailable)
    {
        MarkPlaybackMemoryFailure(errorMessage);
        spdlog::warn("Unable to restore transition history: {}", errorMessage);
        return;
    }

    const bool hadPendingMutations =
        m_pendingTransitionHistory.Size() != 0;
    if (status == PlaybackMemory::StoreLookupStatus::Hit)
    {
        restored.MergeFrom(m_pendingTransitionHistory);
        m_transitionHistory = std::move(restored);
    }
    else
    {
        m_transitionHistory = m_pendingTransitionHistory;
    }
    m_pendingTransitionHistory.Clear();
    m_transitionHistoryReadConclusive = true;
    m_transitionHistoryDirty = hadPendingMutations;
    BumpPlaybackMemoryVersion(m_transitionHistoryVersion);
}

void AudioManager::RetryPendingPlaybackMemoryReads()
{
    PruneRetainedPlaybackMemory();
    ResolveTransitionHistoryRead();
    std::unordered_set<ListeningFatigueData*> attempted;
    const auto retry = [&](const std::shared_ptr<ListeningFatigueData>& memory) {
        if (!memory || memory->readConclusive ||
            !attempted.insert(memory.get()).second)
            return;
        SoundData holder;
        holder.filePath = memory->identity.normalizedPath;
        holder.isMusic = true;
        holder.listeningFatigue = memory;
        LoadPlaybackMemoryForSound(holder, true);
    };
    for (const auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        retry(data.listeningFatigue);
    }
    for (const auto& memory : m_retainedPlaybackMemory) retry(memory);
}

bool AudioManager::HasUnresolvedPlaybackMemoryMutations() const
{
    if (!m_transitionHistoryReadConclusive && m_transitionHistoryDirty)
        return true;
    std::unordered_set<const ListeningFatigueData*> checked;
    const auto unresolved = [&](
        const std::shared_ptr<ListeningFatigueData>& memory) {
        return memory && checked.insert(memory.get()).second &&
            !memory->readConclusive && memory->dirty;
    };
    for (const auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (unresolved(data.listeningFatigue)) return true;
    }
    for (const auto& memory : m_retainedPlaybackMemory)
    {
        if (unresolved(memory)) return true;
    }
    return false;
}

bool AudioManager::RecordListeningCoverage(
    const std::string& soundName,
    double startSeconds,
    double endSeconds,
    double audibilityWeight,
    std::int64_t epochSeconds)
{
    auto sound = m_sounds.find(soundName);
    if (sound == m_sounds.end() || !sound->second.listeningFatigue)
        return false;
    const std::shared_ptr<ListeningFatigueData>& memory =
        sound->second.listeningFatigue;
    if (!ListeningHeatmap::RecordCoverage(
            memory->state,
            startSeconds,
            endSeconds,
        audibilityWeight,
        epochSeconds))
        return false;
    if (!memory->readConclusive)
    {
        if (memory->pendingMutations.bucketFatigue.empty())
        {
            const ListeningHeatmap::State& state = memory->state;
            memory->pendingMutations = ListeningHeatmap::CreateState(
                state.trackDurationSeconds,
                {state.bucketDurationSeconds, state.dailyDecayFactor});
        }
        if (!ListeningHeatmap::RecordCoverage(
                memory->pendingMutations,
                startSeconds,
                endSeconds,
                audibilityWeight,
                epochSeconds))
        {
            MarkPlaybackMemoryFailure(
                "Unable to retain pending playback heatmap mutation.");
            return false;
        }
        memory->hasPendingMutations = true;
    }
    memory->dirty = true;
    BumpPlaybackMemoryVersion(memory->version);
    return true;
}

std::string AudioManager::GetPlaybackMemoryKey(
    const std::string& soundName) const
{
    const auto sound = m_sounds.find(soundName);
    if (sound == m_sounds.end() || !sound->second.listeningFatigue) return {};
    return StableTransitionTrackId(sound->second.listeningFatigue->identity);
}

double AudioManager::GetTransitionDiversityScore(
    const std::string& fromSoundName,
    const std::string& toSoundName,
    std::int64_t epochSeconds) const
{
    const std::string fromKey = GetPlaybackMemoryKey(fromSoundName);
    const std::string toKey = GetPlaybackMemoryKey(toSoundName);
    if (fromKey.empty() || toKey.empty()) return 1.0;
    return m_transitionHistory.GetDiversityScore(
        fromKey, toKey, epochSeconds);
}

bool AudioManager::RecordMusicTransition(
    const std::string& fromSoundName,
    const std::string& toSoundName,
    std::int64_t epochSeconds)
{
    // Transitions occur at segment boundaries, so retrying here is bounded by
    // musical cadence rather than the render/update frame rate.
    ResolveTransitionHistoryRead();
    const std::string fromKey = GetPlaybackMemoryKey(fromSoundName);
    const std::string toKey = GetPlaybackMemoryKey(toSoundName);
    if (fromKey.empty() || toKey.empty()) return false;
    if (!m_transitionHistory.RecordTransition(fromKey, toKey, epochSeconds))
        return false;
    if (!m_transitionHistoryReadConclusive &&
        !m_pendingTransitionHistory.RecordTransition(
            fromKey, toKey, epochSeconds))
    {
        MarkPlaybackMemoryFailure(
            "Unable to retain pending transition-history mutation.");
        return false;
    }
    m_transitionHistoryDirty = true;
    BumpPlaybackMemoryVersion(m_transitionHistoryVersion);
    return true;
}

std::vector<AudioManager::PlaybackMemoryDiagnostic>
AudioManager::GetPlaybackMemoryDiagnostics(std::int64_t epochSeconds) const
{
    std::vector<PlaybackMemoryDiagnostic> diagnostics;
    for (const auto& [soundName, data] : m_sounds)
    {
        if (data.kind != SoundKind::Music || !data.listeningFatigue) continue;
        const ListeningHeatmap::Summary summary = ListeningHeatmap::Summarize(
            data.listeningFatigue->state, epochSeconds);
        diagnostics.push_back({
            soundName,
            data.listeningFatigue->state.bucketFatigue.size(),
            summary.coverage,
            summary.meanFatigue,
            summary.maxFatigue});
    }
    return diagnostics;
}

std::optional<TransitionHistory::TransitionState>
AudioManager::GetTransitionMemory(
    const std::string& fromSoundName,
    const std::string& toSoundName,
    std::int64_t epochSeconds) const
{
    const std::string fromKey = GetPlaybackMemoryKey(fromSoundName);
    const std::string toKey = GetPlaybackMemoryKey(toSoundName);
    if (fromKey.empty() || toKey.empty()) return std::nullopt;
    return m_transitionHistory.GetState(fromKey, toKey, epochSeconds);
}

void AudioManager::RegisterPlaybackMemoryState(
    const std::shared_ptr<ListeningFatigueData>& data)
{
    if (!data || data->identity.normalizedPath.empty()) return;

    const std::string& path = data->identity.normalizedPath;
    const auto currentIt = m_currentPlaybackMemoryByPath.find(path);
    const std::shared_ptr<ListeningFatigueData> previous =
        currentIt == m_currentPlaybackMemoryByPath.end()
        ? nullptr
        : currentIt->second.lock();
    if (previous.get() == data.get()) return;

    BumpPlaybackMemoryVersion(m_nextPlaybackMemoryPathGeneration);
    data->pathGeneration = m_nextPlaybackMemoryPathGeneration;
    m_currentPlaybackMemoryByPath[path] = data;

    // A previous identity for the same normalized_path may still be in an
    // active SQLite job. Force the new generation to write afterwards, even
    // when its initial heatmap is empty, so the stale identity cannot remain
    // the final database row.
    if (previous)
    {
        data->dirty = true;
        BumpPlaybackMemoryVersion(data->version);
    }
    PruneRetainedPlaybackMemory();
}

bool AudioManager::IsCurrentPlaybackMemoryState(
    const std::shared_ptr<ListeningFatigueData>& data) const
{
    if (!data || data->identity.normalizedPath.empty() ||
        data->pathGeneration == 0)
        return false;
    const auto current = m_currentPlaybackMemoryByPath.find(
        data->identity.normalizedPath);
    if (current == m_currentPlaybackMemoryByPath.end()) return false;
    const std::shared_ptr<ListeningFatigueData> owner = current->second.lock();
    return owner.get() == data.get() &&
        owner->pathGeneration == data->pathGeneration;
}

void AudioManager::RetainPlaybackMemoryState(
    const std::shared_ptr<ListeningFatigueData>& data)
{
    if (!data || !data->dirty) return;
    if (!IsCurrentPlaybackMemoryState(data))
    {
        // The database key is the path, so retrying an obsolete identity would
        // overwrite the current file generation. Keep its in-memory heatmap,
        // but do not retain it as pending persistence work.
        data->dirty = false;
        return;
    }
    const auto retained = std::find_if(
        m_retainedPlaybackMemory.begin(),
        m_retainedPlaybackMemory.end(),
        [&](const auto& candidate) { return candidate.get() == data.get(); });
    if (retained == m_retainedPlaybackMemory.end())
        m_retainedPlaybackMemory.push_back(data);
}

void AudioManager::PruneRetainedPlaybackMemory()
{
    for (const auto& data : m_retainedPlaybackMemory)
    {
        if (data && data->dirty && !IsCurrentPlaybackMemoryState(data))
            data->dirty = false;
    }
    std::erase_if(
        m_retainedPlaybackMemory,
        [&](const auto& data) {
            return !data || !data->dirty ||
                !IsCurrentPlaybackMemoryState(data);
        });
    std::erase_if(
        m_currentPlaybackMemoryByPath,
        [](const auto& entry) { return entry.second.expired(); });
}

void AudioManager::CapturePlaybackMemoryFlush(
    PlaybackMemoryFlushSnapshot& snapshot,
    PlaybackMemoryFlushMetadata& metadata)
{
    snapshot = {};
    metadata = {};
    PruneRetainedPlaybackMemory();

    std::unordered_set<const ListeningFatigueData*> captured;
    const auto capture = [&](const std::shared_ptr<ListeningFatigueData>& data) {
        if (!data || !data->readConclusive || !data->dirty ||
            !IsCurrentPlaybackMemoryState(data) ||
            !captured.insert(data.get()).second)
            return;
        snapshot.heatmaps.push_back({
            data->identity,
            data->state,
            data->version,
            data->pathGeneration});
        metadata.heatmaps.push_back({
            data,
            data->version,
            data->pathGeneration});
    };

    for (const auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        capture(data.listeningFatigue);
    }
    for (const auto& data : m_retainedPlaybackMemory) capture(data);

    if (m_transitionHistoryReadConclusive && m_transitionHistoryDirty)
    {
        snapshot.includesTransitionHistory = true;
        snapshot.transitionHistory = m_transitionHistory;
        snapshot.transitionHistoryVersion = m_transitionHistoryVersion;
        metadata.includesTransitionHistory = true;
        metadata.transitionHistoryVersion = m_transitionHistoryVersion;
    }
}

AudioManager::PlaybackMemoryFlushResult
AudioManager::ExecutePlaybackMemoryFlush(
    PlaybackMemory::Store* store,
    PlaybackMemoryFlushSnapshot snapshot)
{
    PlaybackMemoryFlushResult result;
    result.heatmapSucceeded.resize(snapshot.heatmaps.size(), 0);
    result.performedWrite = !snapshot.heatmaps.empty() ||
        snapshot.includesTransitionHistory;
    if (!result.performedWrite) return result;

    if (!store)
    {
        result.allSucceeded = false;
        result.errorMessage = "Playback-memory database is unavailable.";
        return result;
    }

    for (std::size_t index = 0; index < snapshot.heatmaps.size(); ++index)
    {
        const PlaybackMemoryHeatmapSnapshot& heatmap =
            snapshot.heatmaps[index];
        std::string saveError;
        if (store->SaveHeatmap(heatmap.identity, heatmap.state, saveError))
        {
            result.heatmapSucceeded[index] = 1;
        }
        else
        {
            result.allSucceeded = false;
            AppendPlaybackMemoryError(
                result.errorMessage,
                "heatmap '" + heatmap.identity.normalizedPath + "'",
                saveError);
        }
    }

    if (snapshot.includesTransitionHistory)
    {
        std::string saveError;
        result.transitionHistorySucceeded =
            store->SaveTransitionHistory(
                snapshot.transitionHistory, saveError);
        if (!result.transitionHistorySucceeded)
        {
            result.allSucceeded = false;
            AppendPlaybackMemoryError(
                result.errorMessage, "transition history", saveError);
        }
    }
    if (!result.allSucceeded && result.errorMessage.empty())
        result.errorMessage = "Unable to persist playback memory.";
    return result;
}

bool AudioManager::ApplyPlaybackMemoryFlushResult(
    PlaybackMemoryFlushMetadata metadata,
    const PlaybackMemoryFlushResult& result,
    std::string& errorMessage)
{
    errorMessage.clear();
    const bool resultShapeValid =
        result.heatmapSucceeded.size() == metadata.heatmaps.size();
    bool allSucceeded = result.allSucceeded && resultShapeValid;

    for (std::size_t index = 0; index < metadata.heatmaps.size(); ++index)
    {
        PlaybackMemoryHeatmapTarget& target = metadata.heatmaps[index];
        if (!target.data) continue;
        const bool succeeded = resultShapeValid &&
            result.heatmapSucceeded[index] != 0;
        if (succeeded)
        {
            if (target.data->version == target.version &&
                target.data->pathGeneration == target.pathGeneration)
                target.data->dirty = false;
        }
        else
        {
            allSucceeded = false;
            target.data->dirty = true;
            RetainPlaybackMemoryState(target.data);
        }
        // The live map or retained-retry list now owns every state that still
        // matters. Releasing job ownership here also lets expired path entries
        // be purged immediately below.
        target.data.reset();
    }

    if (metadata.includesTransitionHistory)
    {
        if (result.transitionHistorySucceeded)
        {
            if (m_transitionHistoryVersion ==
                metadata.transitionHistoryVersion)
                m_transitionHistoryDirty = false;
        }
        else
        {
            allSucceeded = false;
            m_transitionHistoryDirty = true;
        }
    }

    PruneRetainedPlaybackMemory();
    if (allSucceeded)
    {
        if (result.performedWrite) MarkPlaybackMemoryWriteSuccess();
        return true;
    }

    errorMessage = result.errorMessage.empty()
        ? (resultShapeValid
            ? "Unable to persist playback memory."
            : "Playback-memory worker returned an invalid result.")
        : result.errorMessage;
    MarkPlaybackMemoryFailure(errorMessage, true);
    return false;
}

bool AudioManager::StartPlaybackMemoryFlush(std::string& errorMessage)
{
    errorMessage.clear();
    if (m_activePlaybackMemoryFlushMetadata) return true;
    if (!m_playbackMemoryStoreAvailable || !m_playbackMemoryStore)
    {
        errorMessage = "Playback-memory database is unavailable.";
        MarkPlaybackMemoryFailure(errorMessage);
        return false;
    }

    RetryPendingPlaybackMemoryReads();
    const bool hasUnresolvedMutations =
        HasUnresolvedPlaybackMemoryMutations();

    PlaybackMemoryFlushSnapshot snapshot;
    PlaybackMemoryFlushMetadata metadata;
    try
    {
        CapturePlaybackMemoryFlush(snapshot, metadata);
    }
    catch (const std::exception& exception)
    {
        errorMessage =
            "Unable to snapshot playback memory: " +
            std::string(exception.what());
        MarkPlaybackMemoryFailure(errorMessage, true);
        m_playbackMemoryFlushTimer = 0.0f;
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to snapshot playback memory: unknown error.";
        MarkPlaybackMemoryFailure(errorMessage, true);
        m_playbackMemoryFlushTimer = 0.0f;
        return false;
    }
    m_playbackMemoryFlushTimer = 0.0f;
    if (snapshot.heatmaps.empty() && !snapshot.includesTransitionHistory)
    {
        if (hasUnresolvedMutations)
        {
            errorMessage =
                "Playback-memory reads are not yet conclusive; pending RAM "
                "mutations were not written.";
            MarkPlaybackMemoryFailure(errorMessage);
            return false;
        }
        return true;
    }

    m_activePlaybackMemoryFlushMetadata = std::move(metadata);
    try
    {
        m_playbackMemoryFlushFuture = std::async(
            std::launch::async,
            &AudioManager::ExecutePlaybackMemoryFlush,
            m_playbackMemoryStore.get(),
            std::move(snapshot));
        return true;
    }
    catch (const std::exception& exception)
    {
        PlaybackMemoryFlushResult failed;
        failed.heatmapSucceeded.resize(
            m_activePlaybackMemoryFlushMetadata->heatmaps.size(), 0);
        failed.allSucceeded = false;
        failed.performedWrite = true;
        failed.errorMessage =
            "Unable to start playback-memory worker: " +
            std::string(exception.what());
        PlaybackMemoryFlushMetadata failedMetadata =
            std::move(*m_activePlaybackMemoryFlushMetadata);
        m_activePlaybackMemoryFlushMetadata.reset();
        return ApplyPlaybackMemoryFlushResult(
            std::move(failedMetadata), failed, errorMessage);
    }
    catch (...)
    {
        PlaybackMemoryFlushResult failed;
        failed.heatmapSucceeded.resize(
            m_activePlaybackMemoryFlushMetadata->heatmaps.size(), 0);
        failed.allSucceeded = false;
        failed.performedWrite = true;
        failed.errorMessage =
            "Unable to start playback-memory worker: unknown error.";
        PlaybackMemoryFlushMetadata failedMetadata =
            std::move(*m_activePlaybackMemoryFlushMetadata);
        m_activePlaybackMemoryFlushMetadata.reset();
        return ApplyPlaybackMemoryFlushResult(
            std::move(failedMetadata), failed, errorMessage);
    }
}

bool AudioManager::CompletePlaybackMemoryFlush(
    bool wait,
    bool& completed,
    std::string& errorMessage)
{
    completed = false;
    errorMessage.clear();
    if (!m_activePlaybackMemoryFlushMetadata) return true;
    if (!m_playbackMemoryFlushFuture.valid())
    {
        PlaybackMemoryFlushResult failed;
        failed.heatmapSucceeded.resize(
            m_activePlaybackMemoryFlushMetadata->heatmaps.size(), 0);
        failed.allSucceeded = false;
        failed.performedWrite = true;
        failed.errorMessage = "Playback-memory worker result is unavailable.";
        PlaybackMemoryFlushMetadata metadata =
            std::move(*m_activePlaybackMemoryFlushMetadata);
        m_activePlaybackMemoryFlushMetadata.reset();
        completed = true;
        return ApplyPlaybackMemoryFlushResult(
            std::move(metadata), failed, errorMessage);
    }

    if (!wait && m_playbackMemoryFlushFuture.wait_for(
            std::chrono::seconds(0)) != std::future_status::ready)
        return true;
    if (wait) m_playbackMemoryFlushFuture.wait();

    PlaybackMemoryFlushMetadata metadata =
        std::move(*m_activePlaybackMemoryFlushMetadata);
    m_activePlaybackMemoryFlushMetadata.reset();
    PlaybackMemoryFlushResult result;
    try
    {
        result = m_playbackMemoryFlushFuture.get();
    }
    catch (const std::exception& exception)
    {
        result.heatmapSucceeded.resize(metadata.heatmaps.size(), 0);
        result.allSucceeded = false;
        result.performedWrite = true;
        result.errorMessage =
            "Playback-memory worker failed: " + std::string(exception.what());
    }
    catch (...)
    {
        result.heatmapSucceeded.resize(metadata.heatmaps.size(), 0);
        result.allSucceeded = false;
        result.performedWrite = true;
        result.errorMessage = "Playback-memory worker failed unexpectedly.";
    }
    completed = true;
    const bool succeeded = ApplyPlaybackMemoryFlushResult(
        std::move(metadata), result, errorMessage);
    if (!succeeded) m_playbackMemoryFlushTimer = 0.0f;
    return succeeded;
}

bool AudioManager::FlushPlaybackMemory(std::string& errorMessage)
{
    errorMessage.clear();
    bool completed = false;
    std::string asynchronousError;
    (void)CompletePlaybackMemoryFlush(
        true, completed, asynchronousError);

    if (!m_playbackMemoryStoreAvailable || !m_playbackMemoryStore)
    {
        errorMessage = "Playback-memory database is unavailable.";
        MarkPlaybackMemoryFailure(errorMessage);
        return false;
    }

    RetryPendingPlaybackMemoryReads();
    const bool hasUnresolvedMutations =
        HasUnresolvedPlaybackMemoryMutations();

    PlaybackMemoryFlushSnapshot snapshot;
    PlaybackMemoryFlushMetadata metadata;
    try
    {
        CapturePlaybackMemoryFlush(snapshot, metadata);
    }
    catch (const std::exception& exception)
    {
        errorMessage =
            "Unable to snapshot playback memory: " +
            std::string(exception.what());
        MarkPlaybackMemoryFailure(errorMessage, true);
        return false;
    }
    catch (...)
    {
        errorMessage = "Unable to snapshot playback memory: unknown error.";
        MarkPlaybackMemoryFailure(errorMessage, true);
        return false;
    }
    m_playbackMemoryFlushTimer = 0.0f;
    if (snapshot.heatmaps.empty() && !snapshot.includesTransitionHistory)
    {
        if (!asynchronousError.empty()) errorMessage = asynchronousError;
        if (hasUnresolvedMutations)
        {
            if (!errorMessage.empty()) errorMessage += " | ";
            errorMessage +=
                "Playback-memory reads are not yet conclusive; pending RAM "
                "mutations were not written.";
            MarkPlaybackMemoryFailure(errorMessage);
        }
        return errorMessage.empty();
    }

    const PlaybackMemoryFlushResult result = ExecutePlaybackMemoryFlush(
        m_playbackMemoryStore.get(), std::move(snapshot));
    const bool writesSucceeded = ApplyPlaybackMemoryFlushResult(
        std::move(metadata), result, errorMessage);
    if (hasUnresolvedMutations)
    {
        if (!errorMessage.empty()) errorMessage += " | ";
        errorMessage +=
            "Playback-memory reads are not yet conclusive; pending RAM "
            "mutations were not written.";
        MarkPlaybackMemoryFailure(errorMessage);
        return false;
    }
    return writesSucceeded;
}

bool AudioManager::ClearPlaybackMemory(
    const std::optional<std::string>& soundName,
    std::string& errorMessage)
{
    errorMessage.clear();
    bool flushCompleted = false;
    std::string flushError;
    (void)CompletePlaybackMemoryFlush(
        true, flushCompleted, flushError);
    if (!m_playbackMemoryStoreAvailable || !m_playbackMemoryStore)
    {
        errorMessage = "Playback-memory database is unavailable.";
        MarkPlaybackMemoryFailure(errorMessage);
        return false;
    }

    if (!soundName)
    {
        if (!m_playbackMemoryStore->ClearAll(errorMessage))
        {
            MarkPlaybackMemoryFailure(errorMessage, true);
            return false;
        }
        std::unordered_set<ListeningFatigueData*> reset;
        for (auto& [id, data] : m_sounds)
        {
            (void)id;
            if (!data.listeningFatigue ||
                !reset.insert(data.listeningFatigue.get()).second)
                continue;
            data.listeningFatigue->state = ListeningHeatmap::CreateState(
                data.listeningFatigue->state.trackDurationSeconds);
            data.listeningFatigue->pendingMutations = {};
            data.listeningFatigue->hasPendingMutations = false;
            data.listeningFatigue->readConclusive = true;
            data.listeningFatigue->dirty = false;
            BumpPlaybackMemoryVersion(data.listeningFatigue->version);
        }
        for (const auto& data : m_retainedPlaybackMemory)
        {
            if (!data || !reset.insert(data.get()).second) continue;
            data->state = ListeningHeatmap::CreateState(
                data->state.trackDurationSeconds);
            data->pendingMutations = {};
            data->hasPendingMutations = false;
            data->readConclusive = true;
            data->dirty = false;
            BumpPlaybackMemoryVersion(data->version);
        }
        m_retainedPlaybackMemory.clear();
        m_transitionHistory.Clear();
        m_pendingTransitionHistory.Clear();
        m_transitionHistoryReadConclusive = true;
        m_transitionHistoryDirty = false;
        BumpPlaybackMemoryVersion(m_transitionHistoryVersion);
        ResetPlaybackMemoryHealth();
        for (const auto& [id, data] : m_sounds)
        {
            if (data.isMusic && data.listeningFatigue &&
                data.listeningFatigue->identity.normalizedPath.empty())
            {
                MarkPlaybackMemoryFailure(
                    "Loaded music '" + id +
                    "' has no durable file identity.");
            }
        }
        return true;
    }

    auto sound = m_sounds.find(*soundName);
    if (sound == m_sounds.end() || !sound->second.listeningFatigue)
    {
        errorMessage = "Requested music track was not found.";
        return false;
    }
    RetryPendingPlaybackMemoryReads();
    if (!m_transitionHistoryReadConclusive ||
        !sound->second.listeningFatigue->readConclusive)
    {
        errorMessage =
            "Playback-memory reads are not yet conclusive; track memory was "
            "not cleared.";
        MarkPlaybackMemoryFailure(errorMessage);
        return false;
    }
    const MusicAnalysis::FileIdentity& identity =
        sound->second.listeningFatigue->identity;
    if (!IsCurrentPlaybackMemoryState(sound->second.listeningFatigue))
    {
        errorMessage =
            "Requested music track is not the current file generation for its path.";
        return false;
    }
    const std::string memoryKey = StableTransitionTrackId(identity);
    if (identity.normalizedPath.empty() || memoryKey.empty())
    {
        errorMessage = "Requested music track has no durable file identity.";
        return false;
    }

    TransitionHistory::History remainingHistory = m_transitionHistory;
    (void)remainingHistory.RemoveInvolving(memoryKey);
    if (!m_playbackMemoryStore->ClearTrackMemory(
            identity.normalizedPath, remainingHistory, errorMessage))
    {
        MarkPlaybackMemoryFailure(errorMessage, true);
        return false;
    }
    sound->second.listeningFatigue->state = ListeningHeatmap::CreateState(
        sound->second.listeningFatigue->state.trackDurationSeconds);
    sound->second.listeningFatigue->pendingMutations = {};
    sound->second.listeningFatigue->hasPendingMutations = false;
    sound->second.listeningFatigue->readConclusive = true;
    sound->second.listeningFatigue->dirty = false;
    BumpPlaybackMemoryVersion(sound->second.listeningFatigue->version);
    m_transitionHistory = std::move(remainingHistory);
    m_pendingTransitionHistory.Clear();
    m_transitionHistoryReadConclusive = true;
    m_transitionHistoryDirty = false;
    BumpPlaybackMemoryVersion(m_transitionHistoryVersion);
    PruneRetainedPlaybackMemory();
    bool hasPendingWrite = false;
    for (const auto& [id, data] : m_sounds)
    {
        (void)id;
        if (data.listeningFatigue && data.listeningFatigue->dirty &&
            IsCurrentPlaybackMemoryState(data.listeningFatigue))
        {
            hasPendingWrite = true;
            break;
        }
    }
    if (!hasPendingWrite && m_retainedPlaybackMemory.empty() &&
        !m_transitionHistoryDirty)
        MarkPlaybackMemoryWriteSuccess();
    return true;
}

void AudioManager::MarkPlaybackMemoryFailure(
    const std::string& errorMessage,
    bool retryableWriteFailure)
{
    const std::string diagnostic = errorMessage.empty()
        ? "Playback-memory persistence failed."
        : errorMessage;
    if (retryableWriteFailure)
    {
        m_playbackMemoryWriteFailure = true;
        m_playbackMemoryWriteError = diagnostic;
    }
    else
    {
        if (!m_playbackMemoryStructuralFailure)
        {
            m_playbackMemoryStructuralError = diagnostic;
        }
        else if (m_playbackMemoryStructuralError.find(diagnostic) ==
                 std::string::npos)
        {
            AppendPlaybackMemoryError(
                m_playbackMemoryStructuralError,
                "additional persistence failure",
                diagnostic);
        }
        m_playbackMemoryStructuralFailure = true;
    }
    RefreshPlaybackMemoryHealthDiagnostic();
}

void AudioManager::MarkPlaybackMemoryWriteSuccess()
{
    m_playbackMemoryWriteFailure = false;
    m_playbackMemoryWriteError.clear();
    RefreshPlaybackMemoryHealthDiagnostic();
}

void AudioManager::ResetPlaybackMemoryHealth()
{
    m_playbackMemoryStructuralFailure = false;
    m_playbackMemoryWriteFailure = false;
    m_playbackMemoryStructuralError.clear();
    m_playbackMemoryWriteError.clear();
    RefreshPlaybackMemoryHealthDiagnostic();
}

void AudioManager::RefreshPlaybackMemoryHealthDiagnostic()
{
    m_playbackMemoryPersistenceHealthy = m_playbackMemoryStoreAvailable &&
        !m_playbackMemoryStructuralFailure &&
        !m_playbackMemoryWriteFailure;
    m_playbackMemoryLastError.clear();
    if (m_playbackMemoryStructuralFailure)
        m_playbackMemoryLastError = m_playbackMemoryStructuralError;
    if (m_playbackMemoryWriteFailure)
    {
        if (!m_playbackMemoryLastError.empty())
            m_playbackMemoryLastError += " | ";
        m_playbackMemoryLastError += m_playbackMemoryWriteError;
    }
}

const char* AudioManager::SoundKindToString(SoundKind kind)
{
    switch (kind)
    {
        case SoundKind::SoundEffect: return "sfx";
        case SoundKind::Music: return "music";
        case SoundKind::Announcement: return "announcement";
        case SoundKind::Emergency: return "emergency";
    }
    return "unknown";
}

const char* AudioManager::LoudnessStatusToString(LoudnessStatus status)
{
    switch (status)
    {
        case LoudnessStatus::NotQueued: return "not queued";
        case LoudnessStatus::Queued: return "queued";
        case LoudnessStatus::Analyzing: return "analyzing";
        case LoudnessStatus::Ready: return "ready";
        case LoudnessStatus::Failed: return "failed";
    }
    return "unknown";
}

void AudioManager::SetLoudnessTarget(float targetLufs)
{
    m_loudnessTargetLufs = std::clamp(targetLufs, -30.0f, -8.0f);
    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (!data.isMusic || data.loudnessStatus != LoudnessStatus::Ready) continue;

        const float oldGain = data.normalizationGainLinear;
        float gainDb = std::clamp(m_loudnessTargetLufs - data.integratedLufs, -12.0f, 12.0f);
        if (data.truePeakDb + gainDb > -1.0f) gainDb = -1.0f - data.truePeakDb;
        data.normalizationGainDb = gainDb;
        data.normalizationGainLinear = std::pow(10.0f, gainDb / 20.0f);

        const float ratio = oldGain > 0.0f ? data.normalizationGainLinear / oldGain : 1.0f;
        PruneStoppedChannels(data);
        for (FMOD::Channel* channel : data.channels)
        {
            bool isPlaying = false;
            float volume = 0.0f;
            if (channel && channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
                channel->getVolume(&volume) == FMOD_OK)
            {
                AdjustChannelNormalizationGain(channel, ratio);
            }
        }
    }
}

void AudioManager::StopLoudnessWorker()
{
    m_stopLoudnessThread.store(true);
    m_loudnessCondition.notify_all();
    if (m_loudnessThread.joinable()) m_loudnessThread.join();
    m_loudnessThreadStarted = false;
    m_loudnessWorkerExited.store(false);
    {
        std::lock_guard<std::mutex> lock(m_loudnessMutex);
        m_loudnessTasks.clear();
        m_loudnessResults.clear();
        m_activeLoudnessSound.clear();
    }
    m_stopLoudnessThread.store(false);
}

void AudioManager::ConfigureLoudness(const std::string& cachePath, float targetLufs)
{
    if (m_playbackMemoryStoreAvailable)
    {
        std::string flushError;
        if (!FlushPlaybackMemory(flushError))
        {
            spdlog::warn("Unable to flush previous playback memory: {}", flushError);
            // Keep the current store and its pending read state. Treating a
            // complete live heatmap as a delta for another database would
            // double-count history after a failed flush.
            SetLoudnessTarget(targetLufs);
            return;
        }
    }
    else
    {
        bool flushCompleted = false;
        std::string flushError;
        (void)CompletePlaybackMemoryFlush(
            true, flushCompleted, flushError);
    }
    const bool preserveWriteFailure = m_playbackMemoryWriteFailure;
    const std::string preservedWriteError = m_playbackMemoryWriteError;
    if (m_transitionHistoryReadConclusive)
    {
        if (m_transitionHistoryDirty)
            m_pendingTransitionHistory = m_transitionHistory;
        else
            m_pendingTransitionHistory.Clear();
    }
    m_transitionHistoryReadConclusive = false;

    std::unordered_set<ListeningFatigueData*> preparedHeatmaps;
    const auto prepareHeatmap = [&](
        const std::shared_ptr<ListeningFatigueData>& memory) {
        if (!memory || !preparedHeatmaps.insert(memory.get()).second) return;
        if (memory->readConclusive)
        {
            if (memory->dirty)
            {
                memory->pendingMutations = memory->state;
                memory->hasPendingMutations = true;
            }
            else
            {
                memory->pendingMutations = {};
                memory->hasPendingMutations = false;
            }
        }
        memory->readConclusive = false;
    };
    for (const auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        prepareHeatmap(data.listeningFatigue);
    }
    for (const auto& memory : m_retainedPlaybackMemory)
        prepareHeatmap(memory);
    StopLoudnessWorker();
    m_loudnessCachePath = cachePath.empty() ? "loudness_cache.json" : cachePath;
    std::filesystem::path databasePath = PathFromUtf8(m_loudnessCachePath).parent_path();
    databasePath /= "music_analysis.sqlite3";
    const std::u8string databaseUtf8 = databasePath.generic_u8string();
    m_musicAnalysisDatabasePath.assign(
        reinterpret_cast<const char*>(databaseUtf8.data()), databaseUtf8.size());

    std::filesystem::path playbackMemoryPath =
        PathFromUtf8(m_loudnessCachePath).parent_path() /
        PlaybackMemory::Store::DefaultFileName;
    const std::u8string playbackMemoryUtf8 =
        playbackMemoryPath.generic_u8string();
    m_playbackMemoryDatabasePath.assign(
        reinterpret_cast<const char*>(playbackMemoryUtf8.data()),
        playbackMemoryUtf8.size());
    m_playbackMemoryStore = std::make_unique<PlaybackMemory::Store>(
        playbackMemoryPath);
    std::string playbackMemoryError;
    m_playbackMemoryStoreAvailable =
        m_playbackMemoryStore->Initialize(playbackMemoryError);
    ResetPlaybackMemoryHealth();
    if (!m_playbackMemoryStoreAvailable)
        MarkPlaybackMemoryFailure(playbackMemoryError);
    if (preserveWriteFailure)
        MarkPlaybackMemoryFailure(preservedWriteError, true);
    m_playbackMemoryFlushTimer = 0.0f;
    if (!m_playbackMemoryStoreAvailable)
    {
        spdlog::warn(
            "Playback memory unavailable '{}': {}",
            m_playbackMemoryDatabasePath, playbackMemoryError);
    }
    else
    {
        ResolveTransitionHistoryRead();
        std::unordered_set<ListeningFatigueData*> loadedHeatmaps;
        for (auto& [soundName, data] : m_sounds)
        {
            (void)soundName;
            if (!data.listeningFatigue ||
                !loadedHeatmaps.insert(data.listeningFatigue.get()).second)
                continue;
            if (IsCurrentPlaybackMemoryState(data.listeningFatigue))
            {
                // Load into a temporary and only replace the live state on a
                // conclusive Hit/Miss. A transient read failure must not erase
                // valid in-memory history that was already flushed to the
                // previous store.
                LoadPlaybackMemoryForSound(data, true);
            }
        }
    }
    for (const auto& [soundName, data] : m_sounds)
    {
        if (data.isMusic && data.listeningFatigue &&
            data.listeningFatigue->identity.normalizedPath.empty())
        {
            MarkPlaybackMemoryFailure(
                "Loaded music '" + soundName +
                "' has no durable file identity.");
        }
    }
    m_loudnessTargetLufs = std::clamp(targetLufs, -30.0f, -8.0f);
}

bool AudioManager::ClearLoudnessCache(bool& removed, std::string& errorMessage)
{
    removed = false;
    errorMessage.clear();
    StopLoudnessWorker();

    std::error_code removeError;
    removed = std::filesystem::remove(PathFromUtf8(m_loudnessCachePath), removeError);
    if (removeError)
    {
        errorMessage = removeError.message();
        for (auto& [soundName, data] : m_sounds)
        {
            (void)soundName;
            if (data.loudnessStatus == LoudnessStatus::Queued ||
                data.loudnessStatus == LoudnessStatus::Analyzing)
                data.loudnessStatus = LoudnessStatus::NotQueued;
        }
        return false;
    }

    MusicAnalysis::AnalysisStore analysisStore(
        PathFromUtf8(m_musicAnalysisDatabasePath));
    std::string analysisStoreError;
    if (!analysisStore.Initialize(analysisStoreError) ||
        !analysisStore.Clear(analysisStoreError))
    {
        errorMessage = "Unable to clear smart music analysis cache: " +
            analysisStoreError;
        return false;
    }

    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (data.kind != SoundKind::Music) continue;

        const float oldGain = data.normalizationGainLinear;
        data.loudnessStatus = LoudnessStatus::NotQueued;
        data.integratedLufs = 0.0f;
        data.truePeakDb = 0.0f;
        data.normalizationGainDb = 0.0f;
        data.normalizationGainLinear = 1.0f;
        data.musicAnalysisReady = false;
        data.musicAnalysis = {};

        if (oldGain <= 0.0f || std::abs(oldGain - 1.0f) < 0.000001f) continue;
        const float gainRatio = 1.0f / oldGain;
        PruneStoppedChannels(data);
        for (FMOD::Channel* channel : data.channels)
        {
            bool isPlaying = false;
            float currentVolume = 0.0f;
            if (channel && channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
                channel->getVolume(&currentVolume) == FMOD_OK)
            {
                AdjustChannelNormalizationGain(channel, gainRatio);
            }
        }
    }
    return true;
}

void AudioManager::Shutdown()
{
    if (m_playbackMemoryStoreAvailable)
    {
        std::string flushError;
        if (!FlushPlaybackMemory(flushError))
            spdlog::warn("Unable to flush playback memory at shutdown: {}", flushError);
    }
    else if (m_activePlaybackMemoryFlushMetadata)
    {
        bool flushCompleted = false;
        std::string flushError;
        (void)CompletePlaybackMemoryFlush(
            true, flushCompleted, flushError);
    }
    StopAllSounds();
    StopLoudnessWorker();

    for (auto& [soundName, data] : m_sounds)
    {
        (void)soundName;
        if (data.sound)
        {
            data.sound->release();
            data.sound = nullptr;
        }
        data.channels.clear();
    }
    m_sounds.clear();
    m_channelFades.clear();
    m_playbackMemoryStore.reset();
    m_playbackMemoryStoreAvailable = false;
    m_activePlaybackMemoryFlushMetadata.reset();
    m_playbackMemoryFlushFuture = {};
    m_retainedPlaybackMemory.clear();
    m_currentPlaybackMemoryByPath.clear();
    m_nextPlaybackMemoryPathGeneration = 0;
    ResetPlaybackMemoryHealth();
    m_playbackMemoryFlushTimer = 0.0f;
    m_transitionHistory.Clear();
    m_pendingTransitionHistory.Clear();
    m_transitionHistoryReadConclusive = false;
    m_transitionHistoryDirty = false;
    m_transitionHistoryVersion = 0;

    if (m_musicChannelGroup && m_musicLimiter)
    {
        m_musicChannelGroup->removeDSP(m_musicLimiter);
    }
    if (m_musicLimiter)
    {
        m_musicLimiter->release();
        m_musicLimiter = nullptr;
    }
    if (m_musicChannelGroup)
    {
        m_musicChannelGroup->release();
        m_musicChannelGroup = nullptr;
    }
    if (m_announcementChannelGroup)
    {
        m_announcementChannelGroup->release();
        m_announcementChannelGroup = nullptr;
    }
    if (m_sfxChannelGroup)
    {
        m_sfxChannelGroup->release();
        m_sfxChannelGroup = nullptr;
    }
    if (m_emergencyChannelGroup)
    {
        m_emergencyChannelGroup->release();
        m_emergencyChannelGroup = nullptr;
    }
    m_normalPlaybackBlocked = false;
}

bool AudioManager::EnsureChannelGroup(FMOD::ChannelGroup*& group, const char* name)
{
    if (group) return true;
    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system) return false;
    const FMOD_RESULT result = system->createChannelGroup(name, &group);
    if (result == FMOD_OK && group) return true;
    spdlog::error("Failed to create channel group '{}': {}", name, FMOD_ErrorString(result));
    group = nullptr;
    return false;
}

bool AudioManager::EnsureMusicProcessing()
{
    if (m_musicChannelGroup && m_musicLimiter) return true;

    FMOD::System* system = FModWrapper::GetInstance().GetSystem();
    if (!system) return false;

    FMOD_RESULT result = system->createChannelGroup("TSM Music", &m_musicChannelGroup);
    if (result != FMOD_OK || !m_musicChannelGroup)
    {
        spdlog::error("Failed to create the music channel group: {}", FMOD_ErrorString(result));
        m_musicChannelGroup = nullptr;
        return false;
    }

    result = system->createDSPByType(FMOD_DSP_TYPE_LIMITER, &m_musicLimiter);
    if (result != FMOD_OK || !m_musicLimiter)
    {
        spdlog::error("Failed to create the music limiter: {}", FMOD_ErrorString(result));
        m_musicChannelGroup->release();
        m_musicChannelGroup = nullptr;
        m_musicLimiter = nullptr;
        return false;
    }

    m_musicLimiter->setParameterFloat(FMOD_DSP_LIMITER_RELEASETIME, 50.0f);
    m_musicLimiter->setParameterFloat(FMOD_DSP_LIMITER_CEILING, -1.0f);
    m_musicLimiter->setParameterFloat(FMOD_DSP_LIMITER_MAXIMIZERGAIN, 0.0f);
    m_musicLimiter->setParameterBool(FMOD_DSP_LIMITER_MODE, true);

    result = m_musicChannelGroup->addDSP(0, m_musicLimiter);
    if (result != FMOD_OK)
    {
        spdlog::error("Failed to attach the music limiter: {}", FMOD_ErrorString(result));
        m_musicLimiter->release();
        m_musicChannelGroup->release();
        m_musicLimiter = nullptr;
        m_musicChannelGroup = nullptr;
        return false;
    }

    m_musicLimiter->setActive(true);
    spdlog::info("Music true-peak limiter enabled at -1 dB");
    return true;
}

void AudioManager::StopSoundWithFadeOut(const std::string& soundName)
{
    auto it = m_sounds.find(soundName);
    if (it == m_sounds.end())
    {
        spdlog::warn("Attempted to stop non-existent sound: {}", soundName);
        return;
    }

    PruneStoppedChannels(it->second);
    if (it->second.channels.empty())
    {
        spdlog::warn("No active channels for sound: {}", soundName);
        return;
    }
    
    for (auto* channel : it->second.channels)
    {
        StopChannelWithFadeOut(channel);
    }

    spdlog::info("Starting fade-out for sound: {}", soundName);
}

void AudioManager::StopAllSoundsWithFadeOut()
{
    for (auto& pair : m_sounds)
    {
        if (pair.second.kind == SoundKind::Emergency) continue;
        PruneStoppedChannels(pair.second);
        for (auto* channel : pair.second.channels)
        {
            StopChannelWithFadeOut(channel);
        }
    }
}

void AudioManager::StopChannelWithFadeOut(FMOD::Channel* channel)
{
    if (!channel)
        return;

    bool isPlaying = false;
    if (channel->isPlaying(&isPlaying) != FMOD_OK || !isPlaying)
        return;

    StartChannelFade(channel, 0.0f, true);
}

bool AudioManager::IsChannelFading(FMOD::Channel* channel) const
{
    return std::any_of(m_channelFades.begin(), m_channelFades.end(),
        [channel](const ChannelFade& fade)
        {
            return fade.channel == channel &&
                IsChannelPlayingSound(fade.channel, fade.expectedSound);
        });
}

void AudioManager::StartChannelFade(FMOD::Channel* channel, float targetVolume, bool stopWhenComplete)
{
    if (!channel)
        return;

    bool isPlaying = false;
    FMOD::Sound* sound = nullptr;
    float currentVolume = 0.0f;
    if (channel->isPlaying(&isPlaying) != FMOD_OK || !isPlaying ||
        channel->getCurrentSound(&sound) != FMOD_OK || !sound ||
        channel->getVolume(&currentVolume) != FMOD_OK)
    {
        return;
    }

    CancelChannelFade(channel);
    m_channelFades.push_back(ChannelFade{
        channel,
        sound,
        0.0f,
        m_fadeDuration,
        currentVolume,
        targetVolume,
        stopWhenComplete
    });
}

void AudioManager::RetargetChannelFade(
    FMOD::Channel* channel,
    float targetVolume)
{
    if (!channel || !std::isfinite(targetVolume)) return;
    auto fade = std::find_if(
        m_channelFades.begin(), m_channelFades.end(),
        [channel](const ChannelFade& candidate) {
            return candidate.channel == channel;
        });
    if (fade == m_channelFades.end())
    {
        StartChannelFade(channel, targetVolume, false);
        return;
    }

    // Once a channel has been scheduled to stop, background loudness work is
    // never allowed to resurrect it after logical playlist ownership is gone.
    if (fade->stopWhenComplete) return;

    float currentVolume = 0.0f;
    if (!IsChannelPlayingSound(channel, fade->expectedSound) ||
        channel->getVolume(&currentVolume) != FMOD_OK)
    {
        CancelChannelFade(channel);
        return;
    }
    const float remainingDuration = std::max(
        fade->duration - fade->timer, 0.05f);
    fade->startVolume = currentVolume;
    fade->targetVolume = targetVolume;
    fade->timer = 0.0f;
    fade->duration = remainingDuration;
}

void AudioManager::AdjustChannelNormalizationGain(
    FMOD::Channel* channel,
    float gainRatio)
{
    if (!channel || !std::isfinite(gainRatio) || gainRatio < 0.0f) return;
    const auto fade = std::find_if(
        m_channelFades.begin(), m_channelFades.end(),
        [channel](const ChannelFade& candidate) {
            return candidate.channel == channel;
        });
    if (fade != m_channelFades.end())
    {
        if (fade->stopWhenComplete) return;
        RetargetChannelFade(channel, fade->targetVolume * gainRatio);
        return;
    }

    float currentVolume = 0.0f;
    if (channel->getVolume(&currentVolume) == FMOD_OK)
        RetargetChannelFade(channel, currentVolume * gainRatio);
}

void AudioManager::CancelChannelFade(FMOD::Channel* channel)
{
    m_channelFades.erase(
        std::remove_if(m_channelFades.begin(), m_channelFades.end(),
            [channel](const ChannelFade& fade) { return fade.channel == channel; }),
        m_channelFades.end());
}

void AudioManager::PruneStoppedChannels(SoundData& data)
{
    data.channels.erase(
        std::remove_if(data.channels.begin(), data.channels.end(),
            [&data](FMOD::Channel* channel)
            {
                return !IsChannelPlayingSound(channel, data.sound);
            }),
        data.channels.end());
}

bool AudioManager::IsChannelPlayingSound(
    FMOD::Channel* channel, FMOD::Sound* expectedSound)
{
    if (!channel || !expectedSound) return false;

    bool isPlaying = false;
    FMOD::Sound* currentSound = nullptr;
    return channel->isPlaying(&isPlaying) == FMOD_OK && isPlaying &&
        channel->getCurrentSound(&currentSound) == FMOD_OK &&
        currentSound == expectedSound;
}

bool AudioManager::ApplyPitch(FMOD::Channel* channel, float pitch)
{
    if (!channel || !std::isfinite(pitch) || pitch <= 0.0f)
        return false;
    return channel->setPitch(pitch) == FMOD_OK;
}

} // namespace TSM
