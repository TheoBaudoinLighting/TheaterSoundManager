#pragma once

#include "tsm_runtime.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace TSM
{

inline constexpr int CliSchemaVersion = 1;
inline constexpr std::string_view CliApiVersion = "2.0";

enum class CliExitCode : int
{
    Success = 0,
    Usage = 2,
    InvalidArgument = 3,
    NotFound = 4,
    Conflict = 5,
    Configuration = 6,
    AudioEngine = 7,
    InputOutput = 8,
    Timeout = 9,
    Internal = 10,
    PartialSuccess = 11
};

struct CliResult
{
    CliExitCode exitCode = CliExitCode::Success;
    nlohmann::json data = nlohmann::json::object();
    std::string errorCode;
    std::string errorMessage;
    nlohmann::json errorDetails = nlohmann::json::object();
    bool shutdownRequested = false;

    bool IsSuccess() const { return exitCode == CliExitCode::Success; }

    static CliResult Success(nlohmann::json data = nlohmann::json::object());
    static CliResult Failure(
        CliExitCode exitCode,
        std::string errorCode,
        std::string errorMessage,
        nlohmann::json details = nlohmann::json::object());
};

struct CliInvocation
{
    enum class Mode
    {
        Help,
        Version,
        Schema,
        Execute,
        Serve
    };

    Mode mode = Mode::Help;
    RuntimeOptions runtime;
    std::string command;
    nlohmann::json parameters = nlohmann::json::object();
    nlohmann::json requestId = nullptr;
    bool pretty = false;
    bool quiet = false;
    bool fileLogging = false;
    std::string logFile;
    std::string logLevel = "warn";
    double waitSeconds = 0.0;
    int tickMilliseconds = 10;
};

struct CliParseResult
{
    bool ok = false;
    CliInvocation invocation;
    std::string errorMessage;
};

CliParseResult ParseCliArguments(const std::vector<std::string>& arguments);
std::vector<std::string> GetUtf8CommandLineArguments(int argc, char** argv);
bool IsCliInvocation(const std::vector<std::string>& arguments);

class CliCommandProcessor
{
public:
    explicit CliCommandProcessor(ApplicationRuntime& runtime) : m_runtime(runtime) {}

    CliResult Execute(const std::string& command, const nlohmann::json& parameters);
    static nlohmann::json GetSchema();

private:
    ApplicationRuntime& m_runtime;
};

nlohmann::json BuildCliResponse(
    const CliResult& result,
    const std::string& command,
    const nlohmann::json& requestId = nullptr);
std::string GetCliHelp();
int RunCli(const std::vector<std::string>& arguments);

} // namespace TSM
