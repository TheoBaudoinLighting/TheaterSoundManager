#pragma once

#include "tsm_music_analysis.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace TSM::MusicAnalysis
{

// Stable cache identity for an audio file. Use ReadFileIdentity rather than
// constructing this directly so aliases such as relative paths and ".." do
// not create duplicate cache entries.
struct FileIdentity
{
    std::string normalizedPath;
    std::uint64_t size = 0;
    std::int64_t writeTime = 0;
};

bool ReadFileIdentity(
    const std::filesystem::path& path,
    FileIdentity& identity,
    std::string& errorMessage) noexcept;

enum class StoreLookupStatus
{
    Hit,
    Miss,
    Unavailable
};

// Durable analysis cache backed by the Windows system SQLite library.
// Failures are reported to the caller and never make analysis itself fatal:
// callers can fall back to recomputing a track on Miss or Unavailable.
class AnalysisStore final
{
public:
    static constexpr int CurrentSchemaVersion = 1;

    explicit AnalysisStore(std::filesystem::path databasePath);
    ~AnalysisStore();

    AnalysisStore(const AnalysisStore&) = delete;
    AnalysisStore& operator=(const AnalysisStore&) = delete;
    AnalysisStore(AnalysisStore&&) = delete;
    AnalysisStore& operator=(AnalysisStore&&) = delete;

    bool Initialize(std::string& errorMessage) noexcept;
    StoreLookupStatus Load(
        const FileIdentity& identity,
        TrackAnalysis& analysis,
        std::string& errorMessage) noexcept;
    bool Save(
        const FileIdentity& identity,
        const TrackAnalysis& analysis,
        std::string& errorMessage) noexcept;
    bool Clear(std::string& errorMessage) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace TSM::MusicAnalysis
