#include "tsm_atomic_file.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace TSM
{
namespace
{

std::atomic<std::uint64_t> g_temporaryFileSequence{0};

std::filesystem::path TemporaryPathFor(const std::filesystem::path& target)
{
    std::filesystem::path temporary = target;
#ifdef _WIN32
    const auto processId = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto processId = static_cast<unsigned long long>(getpid());
#endif
    const auto sequence = static_cast<unsigned long long>(
        g_temporaryFileSequence.fetch_add(1, std::memory_order_relaxed) + 1);
    temporary += "." + std::to_string(processId) + "." +
        std::to_string(sequence) + ".tmp";
    return temporary;
}

#ifdef _WIN32
std::string WindowsErrorMessage(DWORD code)
{
    return std::error_code(
        static_cast<int>(code), std::system_category()).message();
}

bool WriteTemporaryFile(
    const std::filesystem::path& path,
    const std::string& payload,
    std::string& errorMessage)
{
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        errorMessage = WindowsErrorMessage(GetLastError());
        return false;
    }

    bool success = true;
    std::size_t written = 0;
    while (written < payload.size())
    {
        const std::size_t remaining = payload.size() - written;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, (std::numeric_limits<DWORD>::max)()));
        DWORD chunkWritten = 0;
        if (!WriteFile(file, payload.data() + written, chunk, &chunkWritten, nullptr) ||
            chunkWritten != chunk)
        {
            errorMessage = WindowsErrorMessage(GetLastError());
            success = false;
            break;
        }
        written += chunkWritten;
    }

    if (success && !FlushFileBuffers(file))
    {
        errorMessage = WindowsErrorMessage(GetLastError());
        success = false;
    }
    if (!CloseHandle(file) && success)
    {
        errorMessage = WindowsErrorMessage(GetLastError());
        success = false;
    }
    return success;
}
#else
bool WriteTemporaryFile(
    const std::filesystem::path& path,
    const std::string& payload,
    std::string& errorMessage)
{
    const int descriptor = open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
    {
        errorMessage = std::error_code(errno, std::generic_category()).message();
        return false;
    }

    bool success = true;
    std::size_t written = 0;
    while (written < payload.size())
    {
        const ssize_t result = write(
            descriptor, payload.data() + written, payload.size() - written);
        if (result < 0)
        {
            if (errno == EINTR) continue;
            errorMessage = std::error_code(errno, std::generic_category()).message();
            success = false;
            break;
        }
        if (result == 0)
        {
            errorMessage = "write returned zero bytes";
            success = false;
            break;
        }
        written += static_cast<std::size_t>(result);
    }

    if (success && fsync(descriptor) != 0)
    {
        errorMessage = std::error_code(errno, std::generic_category()).message();
        success = false;
    }
    if (close(descriptor) != 0 && success)
    {
        errorMessage = std::error_code(errno, std::generic_category()).message();
        success = false;
    }
    return success;
}
#endif

} // namespace

JsonFileReadResult ReadJsonFileStrict(
    const std::filesystem::path& path,
    std::size_t maximumBytes)
{
    JsonFileReadResult result;
    if (path.empty())
    {
        result.status = JsonFileReadStatus::Invalid;
        result.error = "JSON file path is empty.";
        return result;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error)
    {
        result.status = JsonFileReadStatus::InputOutput;
        result.error = error.message();
        return result;
    }
    if (!exists)
    {
        result.status = JsonFileReadStatus::Missing;
        result.error = "JSON file does not exist.";
        return result;
    }
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        result.status = error
            ? JsonFileReadStatus::InputOutput
            : JsonFileReadStatus::Invalid;
        result.error = error ? error.message() : "JSON path is not a regular file.";
        return result;
    }

    const std::uintmax_t fileSize = std::filesystem::file_size(path, error);
    if (error)
    {
        result.status = JsonFileReadStatus::InputOutput;
        result.error = error.message();
        return result;
    }
    if (fileSize == 0 || fileSize > maximumBytes)
    {
        result.status = JsonFileReadStatus::Invalid;
        result.error = fileSize == 0
            ? "JSON file is empty."
            : "JSON file exceeds the configured size limit.";
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        result.status = JsonFileReadStatus::InputOutput;
        result.error = "Unable to open JSON file.";
        return result;
    }

    std::string payload(static_cast<std::size_t>(fileSize), '\0');
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (input.gcount() != static_cast<std::streamsize>(payload.size()) || input.bad())
    {
        result.status = JsonFileReadStatus::InputOutput;
        result.error = "Unable to read the complete JSON file.";
        return result;
    }

    try
    {
        result.document = nlohmann::json::parse(payload);
        result.status = JsonFileReadStatus::Ok;
        result.error.clear();
    }
    catch (const std::exception& exception)
    {
        result.status = JsonFileReadStatus::Invalid;
        result.error = exception.what();
    }
    return result;
}

bool WriteJsonFileAtomically(
    const std::filesystem::path& path,
    const nlohmann::json& document,
    std::string& errorMessage)
{
    errorMessage.clear();
    if (path.empty())
    {
        errorMessage = "JSON file path is empty.";
        return false;
    }

    std::filesystem::path temporaryPath;
    try
    {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError)
            {
                errorMessage = directoryError.message();
                return false;
            }
        }

        std::string payload = document.dump(2);
        payload.push_back('\n');
        temporaryPath = TemporaryPathFor(path);
        if (!WriteTemporaryFile(temporaryPath, payload, errorMessage))
        {
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }

#ifdef _WIN32
        if (!MoveFileExW(
                temporaryPath.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            errorMessage = WindowsErrorMessage(GetLastError());
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }
#else
        if (rename(temporaryPath.c_str(), path.c_str()) != 0)
        {
            errorMessage = std::error_code(errno, std::generic_category()).message();
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }

        const std::filesystem::path directory = parent.empty() ? "." : parent;
        const int directoryDescriptor = open(directory.c_str(), O_RDONLY | O_CLOEXEC);
        if (directoryDescriptor >= 0)
        {
            if (fsync(directoryDescriptor) != 0)
                errorMessage = std::error_code(errno, std::generic_category()).message();
            close(directoryDescriptor);
        }
        if (!errorMessage.empty()) return false;
#endif
        return true;
    }
    catch (const std::exception& exception)
    {
        errorMessage = exception.what();
    }
    catch (...)
    {
        errorMessage = "Unknown atomic JSON write failure.";
    }

    if (!temporaryPath.empty())
    {
        std::error_code ignored;
        std::filesystem::remove(temporaryPath, ignored);
    }
    return false;
}

} // namespace TSM
