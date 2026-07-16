#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace TSM
{

enum class JsonFileReadStatus
{
    Ok,
    Missing,
    Invalid,
    InputOutput
};

struct JsonFileReadResult
{
    JsonFileReadStatus status = JsonFileReadStatus::InputOutput;
    nlohmann::json document;
    std::string error;

    bool IsSuccess() const { return status == JsonFileReadStatus::Ok; }
};

// Reads one bounded JSON document. Empty files, trailing non-whitespace data,
// non-regular files and oversized files are rejected.
JsonFileReadResult ReadJsonFileStrict(
    const std::filesystem::path& path,
    std::size_t maximumBytes = 1024U * 1024U);

// Writes a complete JSON document to a temporary file in the target directory,
// flushes it to stable storage, then atomically replaces the destination.
bool WriteJsonFileAtomically(
    const std::filesystem::path& path,
    const nlohmann::json& document,
    std::string& errorMessage);

} // namespace TSM
