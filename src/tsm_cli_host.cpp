#include "tsm_cli.h"

#include "tsm_logger.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace TSM
{
namespace
{

#ifndef TSM_VERSION
#define TSM_VERSION "dev"
#endif

constexpr std::size_t MaximumRequestBytes = 1024 * 1024;
std::atomic<bool> g_interrupted{false};

class CliHostError final : public std::runtime_error
{
public:
    CliHostError(CliExitCode exitCode, std::string code, std::string message)
        : std::runtime_error(std::move(message)),
          exitCode(exitCode),
          code(std::move(code))
    {
    }

    CliExitCode exitCode;
    std::string code;
};

#ifdef _WIN32
std::atomic<HANDLE> g_mainThreadHandle{nullptr};

void CancelMainInput()
{
    if (const HANDLE thread = g_mainThreadHandle.load()) CancelSynchronousIo(thread);
}

BOOL WINAPI ConsoleControlHandler(DWORD controlType)
{
    if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT ||
        controlType == CTRL_CLOSE_EVENT)
    {
        g_interrupted.store(true);
        CancelMainInput();
        return TRUE;
    }
    return FALSE;
}

class ConsoleControlGuard
{
public:
    ConsoleControlGuard()
    {
        HANDLE thread = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            m_thread = thread;
            g_mainThreadHandle.store(thread);
        }
        SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);
    }

    ~ConsoleControlGuard()
    {
        SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
        g_mainThreadHandle.store(nullptr);
        if (m_thread) CloseHandle(m_thread);
    }

private:
    HANDLE m_thread = nullptr;
};
#endif

spdlog::level::level_enum ParseLogLevel(const std::string& value)
{
    if (value == "trace") return spdlog::level::trace;
    if (value == "debug") return spdlog::level::debug;
    if (value == "info") return spdlog::level::info;
    if (value == "warn" || value == "warning") return spdlog::level::warn;
    if (value == "error") return spdlog::level::err;
    if (value == "critical") return spdlog::level::critical;
    if (value == "off") return spdlog::level::off;
    throw std::invalid_argument(
        "--log-level must be trace, debug, info, warn, error, critical, or off.");
}

void InitializeCliLogger(const CliInvocation& invocation)
{
    Logger::Options options;
    options.console = invocation.quiet
        ? Logger::ConsoleTarget::Disabled
        : Logger::ConsoleTarget::Stderr;
    options.fileLogging = invocation.fileLogging;
    options.filePath = invocation.logFile.empty() ? "tsm-cli.log" : invocation.logFile;
    options.level = ParseLogLevel(invocation.logLevel);
    try
    {
        Logger::Init(options);
    }
    catch (const std::exception& error)
    {
        throw CliHostError(
            CliExitCode::InputOutput,
            "log_initialization_failed",
            std::string("Unable to initialize CLI logging: ") + error.what());
    }
}

void WriteJson(const nlohmann::json& value, bool pretty)
{
    std::cout << value.dump(pretty ? 2 : -1) << '\n';
    std::cout.flush();
    if (!std::cout)
        throw CliHostError(
            CliExitCode::InputOutput,
            "stdout_write_failed",
            "Unable to write the CLI response to stdout.");
}

nlohmann::json ParametersWithConfigDefault(
    const CliInvocation& invocation,
    const std::string& command,
    nlohmann::json parameters)
{
    if ((command == "config.validate" || command == "config.show") &&
        !parameters.contains("path"))
    {
        parameters["path"] = invocation.runtime.configPath;
    }
    return parameters;
}

bool CanRunWithoutRuntime(const std::string& command)
{
    return command == "system.ping" || command == "system.capabilities" ||
           command == "config.validate" || command == "config.show";
}

bool IsKnownCommand(const std::string& command)
{
    static const std::set<std::string> commands = [] {
        std::set<std::string> result;
        const nlohmann::json schema = CliCommandProcessor::GetSchema();
        for (const auto& entry : schema.at("commands"))
            result.insert(entry.at("name").get<std::string>());
        return result;
    }();
    return commands.contains(command);
}

bool RequiresPersistentRuntime(const std::string& command)
{
    return command == "sound.play" || command == "playlist.play" ||
           command == "playlist.play-index" || command == "library.play" ||
           command == "announcement.play" ||
           command == "loudness.analyze";
}

bool RequiresServer(const std::string& command, const nlohmann::json& parameters)
{
    if (command == "playlist.options")
    {
        static const std::set<std::string_view> mutationParameters = {
            "random_order", "random_segment", "segment_duration",
            "automatic_segment_duration", "min_segment_duration",
            "max_segment_duration", "loop", "crossfade"};
        for (const std::string_view parameter : mutationParameters)
        {
            if (parameters.contains(parameter)) return true;
        }
        return false;
    }
    if (command == "loudness.target") return parameters.contains("value");

    static const std::set<std::string_view> commands = {
        "system.shutdown", "config.reload",
        "safety.heartbeat", "safety.trip", "safety.reset",
        "session.resume", "session.discard",
        "sound.load", "sound.unload", "sound.stop", "sound.stop-all",
        "sound.set", "sound.pause", "sound.resume", "sound.seek",
        "playlist.create", "playlist.delete", "playlist.rename",
        "playlist.duplicate", "playlist.add", "playlist.remove",
        "playlist.clear", "playlist.move",
        "playlist.import", "playlist.load", "playlist.stop", "playlist.next",
        "library.stop", "library.next", "library.clear-history",
        "announcement.load", "announcement.unload", "announcement.stop",
        "schedule.add", "schedule.update", "schedule.remove", "schedule.reset",
        "mixer.set"
    };
    return commands.contains(command);
}

CliResult RuntimeInitializationFailure(
    const ApplicationRuntime& runtime, const std::string& error)
{
    switch (runtime.GetLastErrorKind())
    {
        case RuntimeErrorKind::Configuration:
            return CliResult::Failure(
                CliExitCode::Configuration,
                "runtime_configuration_failed",
                error);
        case RuntimeErrorKind::InputOutput:
            return CliResult::Failure(
                CliExitCode::InputOutput,
                "runtime_io_failed",
                error);
        case RuntimeErrorKind::None:
        case RuntimeErrorKind::AudioEngine:
            return CliResult::Failure(
                CliExitCode::AudioEngine,
                "audio_initialization_failed",
                error);
    }
    return CliResult::Failure(CliExitCode::Internal, "internal_error", error);
}

int RunOneShot(const CliInvocation& invocation)
{
    InitializeCliLogger(invocation);
    ApplicationRuntime runtime;
    CliCommandProcessor processor(runtime);
    nlohmann::json parameters = ParametersWithConfigDefault(
        invocation, invocation.command, invocation.parameters);

    if (!IsKnownCommand(invocation.command))
    {
        const CliResult result = processor.Execute(invocation.command, parameters);
        WriteJson(
            BuildCliResponse(result, invocation.command, invocation.requestId),
            invocation.pretty);
        Logger::Shutdown();
        return static_cast<int>(result.exitCode);
    }

    if (RequiresServer(invocation.command, parameters))
    {
        const CliResult error = CliResult::Failure(
            CliExitCode::Conflict,
            "persistent_host_required",
            "This command mutates process state and must be sent to '--cli serve'.",
            {{"command", invocation.command}});
        WriteJson(
            BuildCliResponse(error, invocation.command, invocation.requestId),
            invocation.pretty);
        Logger::Shutdown();
        return static_cast<int>(error.exitCode);
    }

    if (RequiresPersistentRuntime(invocation.command) && invocation.waitSeconds <= 0.0)
    {
        const CliResult error = CliResult::Failure(
            CliExitCode::Conflict,
            "persistent_runtime_required",
            "This command controls live audio. Use 'serve' or provide --wait SECONDS.",
            {{"command", invocation.command}});
        WriteJson(
            BuildCliResponse(error, invocation.command, invocation.requestId),
            invocation.pretty);
        Logger::Shutdown();
        return static_cast<int>(error.exitCode);
    }

    if (!CanRunWithoutRuntime(invocation.command))
    {
        std::string error;
        if (!runtime.Initialize(invocation.runtime, error))
        {
            const CliResult failure = RuntimeInitializationFailure(runtime, error);
            WriteJson(
                BuildCliResponse(failure, invocation.command, invocation.requestId),
                invocation.pretty);
            Logger::Shutdown();
            return static_cast<int>(failure.exitCode);
        }
    }

    CliResult result = processor.Execute(invocation.command, parameters);
    if (result.IsSuccess() && invocation.waitSeconds > 0.0 && runtime.IsInitialized())
    {
        const auto start = std::chrono::steady_clock::now();
        auto previous = start;
        const auto deadline = start + std::chrono::duration<double>(invocation.waitSeconds);
        while (!g_interrupted.load() && std::chrono::steady_clock::now() < deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            runtime.Tick(std::chrono::duration<float>(now - previous).count());
            previous = now;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(invocation.tickMilliseconds));
        }
        result.data["waitedSeconds"] = invocation.waitSeconds;
        result.data["interrupted"] = g_interrupted.load();
    }

    WriteJson(
        BuildCliResponse(result, invocation.command, invocation.requestId),
        invocation.pretty);
    runtime.Shutdown();
    Logger::Shutdown();
    if (g_interrupted.load()) return 130;
    return static_cast<int>(result.exitCode);
}

CliResult ParseAndExecuteRequest(
    const std::string& line,
    CliCommandProcessor& processor,
    nlohmann::json& requestId,
    std::string& command)
{
    if (line.size() > MaximumRequestBytes)
    {
        return CliResult::Failure(
            CliExitCode::Usage,
            "request_too_large",
            "NDJSON request exceeds the 1 MiB limit.",
            {{"maximumBytes", MaximumRequestBytes}});
    }

    nlohmann::json request;
    try
    {
        request = nlohmann::json::parse(line);
    }
    catch (const std::exception& error)
    {
        return CliResult::Failure(
            CliExitCode::Usage,
            "invalid_json",
            error.what());
    }
    if (!request.is_object())
    {
        return CliResult::Failure(
            CliExitCode::Usage,
            "invalid_request",
            "NDJSON request must be an object.");
    }
    if (request.contains("id"))
    {
        const auto& candidateId = request["id"];
        if (candidateId.is_array() || candidateId.is_object())
        {
            return CliResult::Failure(
                CliExitCode::Usage,
                "invalid_request_id",
                "Request ID must be a string, number, boolean, or null.");
        }
        requestId = candidateId;
    }
    if (!request.contains("command") || !request["command"].is_string() ||
        request["command"].get_ref<const std::string&>().empty())
    {
        return CliResult::Failure(
            CliExitCode::Usage,
            "missing_command",
            "Request field 'command' must be a non-empty string.");
    }
    command = request["command"].get<std::string>();
    const nlohmann::json parameters = request.value("params", nlohmann::json::object());
    if (!parameters.is_object())
    {
        return CliResult::Failure(
            CliExitCode::Usage,
            "invalid_params",
            "Request field 'params' must be an object.");
    }
    return processor.Execute(command, parameters);
}

enum class LineReadResult
{
    Line,
    EndOfFile,
    Error
};

LineReadResult ReadBoundedLine(
    std::istream& input, std::string& line, bool& tooLarge)
{
    line.clear();
    tooLarge = false;
    bool receivedData = false;
    char character = '\0';
    while (input.get(character))
    {
        receivedData = true;
        if (character == '\n')
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return LineReadResult::Line;
        }
        if (line.size() < MaximumRequestBytes)
            line.push_back(character);
        else
            tooLarge = true;
    }
    if (input.eof())
    {
        if (receivedData)
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return LineReadResult::Line;
        }
        return LineReadResult::EndOfFile;
    }
    return LineReadResult::Error;
}

int RunServer(const CliInvocation& invocation)
{
    if (invocation.pretty)
    {
        const CliResult failure = CliResult::Failure(
            CliExitCode::Usage,
            "pretty_not_supported",
            "--pretty is incompatible with the one-record-per-line NDJSON protocol.");
        WriteJson(BuildCliResponse(failure, "system.ready"), false);
        return static_cast<int>(failure.exitCode);
    }

    InitializeCliLogger(invocation);
    ApplicationRuntime runtime;
    std::string error;
    if (!runtime.Initialize(invocation.runtime, error))
    {
        const CliResult failure = RuntimeInitializationFailure(runtime, error);
        WriteJson(BuildCliResponse(failure, "system.ready"), false);
        Logger::Shutdown();
        return static_cast<int>(failure.exitCode);
    }

    CliCommandProcessor processor(runtime);
    const CliResult status = processor.Execute("system.status", nlohmann::json::object());
    WriteJson({
        {"schemaVersion", CliSchemaVersion},
        {"apiVersion", CliApiVersion},
        {"event", "ready"},
        {"data", status.data}
    }, false);

    std::mutex runtimeMutex;
    std::mutex tickFailureMutex;
    std::exception_ptr tickFailure;
    std::jthread tickThread([&](std::stop_token stopToken) {
        try
        {
            auto previous = std::chrono::steady_clock::now();
            const auto tickDuration =
                std::chrono::milliseconds(invocation.tickMilliseconds);
            while (!stopToken.stop_requested() && !g_interrupted.load())
            {
                const auto started = std::chrono::steady_clock::now();
                const float deltaTime =
                    std::chrono::duration<float>(started - previous).count();
                previous = started;
                {
                    std::lock_guard<std::mutex> lock(runtimeMutex);
                    runtime.Tick(deltaTime);
                }
                std::this_thread::sleep_until(started + tickDuration);
            }
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lock(tickFailureMutex);
                tickFailure = std::current_exception();
            }
            g_interrupted.store(true);
#ifdef _WIN32
            CancelMainInput();
#endif
        }
    });

    std::string line;
    while (!g_interrupted.load())
    {
        bool tooLarge = false;
        const LineReadResult readResult = ReadBoundedLine(std::cin, line, tooLarge);
        if (readResult == LineReadResult::EndOfFile) break;
        if (readResult == LineReadResult::Error && g_interrupted.load()) break;
        if (readResult == LineReadResult::Error)
            throw CliHostError(
                CliExitCode::InputOutput,
                "stdin_read_failed",
                "Unable to read an NDJSON request from stdin.");
        if (line.empty()) continue;
        nlohmann::json requestId = nullptr;
        std::string command;
        CliResult result;
        if (tooLarge)
        {
            result = CliResult::Failure(
                CliExitCode::Usage,
                "request_too_large",
                "NDJSON request exceeds the 1 MiB limit.",
                {{"maximumBytes", MaximumRequestBytes}});
        }
        else
        {
            std::lock_guard<std::mutex> lock(runtimeMutex);
            result = ParseAndExecuteRequest(line, processor, requestId, command);
        }
        WriteJson(BuildCliResponse(result, command, requestId), false);
        if (result.shutdownRequested) break;
    }

    tickThread.request_stop();
    if (tickThread.joinable()) tickThread.join();
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        runtime.Shutdown();
    }
    Logger::Shutdown();
    {
        std::lock_guard<std::mutex> lock(tickFailureMutex);
        if (tickFailure)
            throw CliHostError(
                CliExitCode::Internal,
                "runtime_tick_failed",
                "The runtime tick thread stopped unexpectedly.");
    }
    return g_interrupted.load() ? 130 : 0;
}

} // namespace

std::string GetCliHelp()
{
    return R"(Theater Sound Manager CLI

Usage:
  TheaterSoundManager.exe --cli <resource> <action> [arguments] [options]
  TheaterSoundManager.exe --cli serve [global options]
  TheaterSoundManager.exe --cli --request '{"command":"system.status","params":{}}'

Global options:
  --config PATH       Configuration file (auto-detected by default)
  --no-config         Start an empty session
  --state-dir PATH    Durable analysis, exploration, recovery, and safety state
  --no-sound          Use FMOD's no-sound output (testing/automation)
  --bluetooth         Attempt to start the Bluetooth server in this process
  --wait SECONDS      Keep a one-shot playback/analysis command alive
  --tick-ms N         Runtime tick interval, 1-1000 ms (default: 10)
  --pretty            Pretty-print one-shot JSON
  --quiet             Disable CLI console logs (stdout is always JSON-only)
  --log-level LEVEL   trace|debug|info|warn|error|critical|off
  --log-file PATH     Also write logs to a file
  --schema            Print the versioned command schema
  --version           Print application and CLI versions

Command groups:
  system        ping, status, health, capabilities, shutdown
  config        validate, show, reload
  cinema        status, schedules, preview
  safety        status, heartbeat, trip, reset
  session       status, resume, discard
  sound         list, show, load, unload, play, stop, stop-all,
                set, pause, resume, seek
  playlist      list, show, create, delete, rename, duplicate, add,
                remove, clear, move, options, import, export, save,
                load, play, play-index, stop, next, status
  library       play, stop, next, status, history, clear-history
                (all imported music)
  announcement  list, load, unload, play, stop, status
  schedule      list, add, update, remove, reset
  mixer         get, set
  loudness      status, analyze, target, clear-cache

Examples:
  TheaterSoundManager.exe --cli config validate --check-files
  TheaterSoundManager.exe --cli sound list --kind music --pretty
  TheaterSoundManager.exe --cli playlist show playlist_PreShow --pretty
  TheaterSoundManager.exe --cli playlist play playlist_PreShow --wait 30
  TheaterSoundManager.exe --cli library play --random-order --wait 30

Persistent integration protocol:
  Start with '--cli serve'. The first stdout line is a ready event. Then send
  one JSON object per line on stdin and read one response per line on stdout:

  {"id":1,"command":"playlist.play","params":{"name":"playlist_PreShow"}}
  {"id":2,"command":"system.status","params":{}}
  {"id":3,"command":"system.shutdown","params":{}}

  In-memory mutations such as mixer.set and schedule.add require this mode.

See docs/CLI.md for the full contract or '--cli --schema' for the versioned catalog.
)";
}

int RunCli(const std::vector<std::string>& arguments)
{
    g_interrupted.store(false);
#ifdef _WIN32
    const ConsoleControlGuard consoleControlGuard;
#endif

    const CliParseResult parsed = ParseCliArguments(arguments);
    if (!parsed.ok)
    {
        const CliResult failure = CliResult::Failure(
            CliExitCode::Usage,
            "invalid_invocation",
            parsed.errorMessage);
        try
        {
            WriteJson(BuildCliResponse(failure, ""), false);
        }
        catch (...)
        {
            return static_cast<int>(CliExitCode::InputOutput);
        }
        return static_cast<int>(failure.exitCode);
    }

    const CliInvocation& invocation = parsed.invocation;
    try
    {
        if (invocation.mode == CliInvocation::Mode::Help)
        {
            std::cout << GetCliHelp();
            if (!std::cout) return static_cast<int>(CliExitCode::InputOutput);
            return 0;
        }
        if (invocation.mode == CliInvocation::Mode::Version)
        {
            WriteJson({
                {"name", "TheaterSoundManager"},
                {"version", TSM_VERSION},
                {"cliApiVersion", CliApiVersion},
                {"schemaVersion", CliSchemaVersion}
            }, invocation.pretty);
            return 0;
        }
        if (invocation.mode == CliInvocation::Mode::Schema)
        {
            WriteJson(CliCommandProcessor::GetSchema(), invocation.pretty);
            return 0;
        }
        if (invocation.mode == CliInvocation::Mode::Serve)
            return RunServer(invocation);
        return RunOneShot(invocation);
    }
    catch (const CliHostError& error)
    {
        Logger::Shutdown();
        const CliResult failure = CliResult::Failure(
            error.exitCode, error.code, error.what());
        try
        {
            WriteJson(
                BuildCliResponse(failure, invocation.command, invocation.requestId),
                invocation.pretty);
        }
        catch (...)
        {
        }
        return static_cast<int>(error.exitCode);
    }
    catch (const std::invalid_argument& error)
    {
        const CliResult failure = CliResult::Failure(
            CliExitCode::Usage,
            "invalid_invocation",
            error.what());
        Logger::Shutdown();
        try
        {
            WriteJson(
                BuildCliResponse(failure, invocation.command, invocation.requestId),
                invocation.pretty);
        }
        catch (...)
        {
            return static_cast<int>(CliExitCode::InputOutput);
        }
        return static_cast<int>(failure.exitCode);
    }
    catch (const std::exception& error)
    {
        const CliResult failure = CliResult::Failure(
            CliExitCode::Internal,
            "internal_error",
            error.what());
        Logger::Shutdown();
        try
        {
            WriteJson(
                BuildCliResponse(failure, invocation.command, invocation.requestId),
                invocation.pretty);
        }
        catch (...)
        {
            return static_cast<int>(CliExitCode::InputOutput);
        }
        return static_cast<int>(failure.exitCode);
    }
}

} // namespace TSM
