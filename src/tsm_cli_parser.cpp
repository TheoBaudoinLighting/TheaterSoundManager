#include "tsm_cli.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace TSM
{
namespace
{

using Positionals = std::map<std::string, std::vector<std::string>>;

const Positionals& CommandPositionals()
{
    static const Positionals commands = {
        {"system.ping", {}},
        {"system.status", {}},
        {"system.health", {}},
        {"system.capabilities", {}},
        {"system.shutdown", {}},
        {"config.validate", {"path"}},
        {"config.show", {"path"}},
        {"config.reload", {"path"}},
        {"cinema.status", {}},
        {"cinema.schedules", {}},
        {"cinema.preview", {"at"}},
        {"safety.status", {}},
        {"safety.heartbeat", {"source_id", "session_id", "sequence", "state"}},
        {"safety.trip", {"cause", "source"}},
        {"safety.reset", {"incident_id", "operator", "reason"}},
        {"session.status", {}},
        {"session.resume", {}},
        {"session.discard", {}},
        {"sound.list", {}},
        {"sound.show", {"id"}},
        {"sound.load", {"id", "path"}},
        {"sound.unload", {"id"}},
        {"sound.play", {"id"}},
        {"sound.stop", {"id"}},
        {"sound.stop-all", {}},
        {"sound.set", {"id"}},
        {"sound.pause", {"id"}},
        {"sound.resume", {"id"}},
        {"sound.seek", {"id", "position_ms"}},
        {"playlist.list", {}},
        {"playlist.show", {"name"}},
        {"playlist.create", {"name"}},
        {"playlist.delete", {"name"}},
        {"playlist.rename", {"name", "new_name"}},
        {"playlist.duplicate", {"name", "new_name"}},
        {"playlist.add", {"name", "id"}},
        {"playlist.remove", {"name", "id"}},
        {"playlist.clear", {"name"}},
        {"playlist.move", {"name", "from", "to"}},
        {"playlist.options", {"name"}},
        {"playlist.import", {"path", "name"}},
        {"playlist.export", {"name", "path"}},
        {"playlist.save", {"path"}},
        {"playlist.load", {"path"}},
        {"playlist.play", {"name"}},
        {"playlist.play-index", {"name", "index"}},
        {"playlist.stop", {"name"}},
        {"playlist.next", {"name"}},
        {"playlist.status", {"name"}},
        {"library.play", {}},
        {"library.stop", {}},
        {"library.next", {}},
        {"library.status", {}},
        {"library.history", {"id"}},
        {"library.clear-history", {"id"}},
        {"announcement.list", {}},
        {"announcement.load", {"id", "path"}},
        {"announcement.unload", {"id"}},
        {"announcement.play", {"id"}},
        {"announcement.stop", {}},
        {"announcement.status", {}},
        {"schedule.list", {}},
        {"schedule.add", {"announcement"}},
        {"schedule.update", {"schedule_id"}},
        {"schedule.remove", {"schedule_id"}},
        {"schedule.reset", {}},
        {"mixer.get", {}},
        {"mixer.set", {}},
        {"loudness.status", {"id"}},
        {"loudness.analyze", {"id"}},
        {"loudness.target", {}},
        {"loudness.clear-cache", {}}
    };
    return commands;
}

bool IsBooleanCommandOption(std::string_view key)
{
    static const std::set<std::string_view> options = {
        "check_files", "stream", "loop", "fade_in", "fade",
        "random_order", "random_segment", "automatic_segment_duration",
        "sfx_before", "sfx_after", "include_buckets", "play_evacuation"
    };
    return options.contains(key);
}

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string NormalizeKey(std::string key)
{
    std::replace(key.begin(), key.end(), '-', '_');
    return key;
}

bool ParseDoubleStrict(const std::string& value, double& result)
{
    try
    {
        std::size_t consumed = 0;
        result = std::stod(value, &consumed);
        return consumed == value.size() && std::isfinite(result);
    }
    catch (...)
    {
        return false;
    }
}

bool ParseIntStrict(const std::string& value, int& result)
{
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

std::string CanonicalCommand(std::string resource, const std::string& action)
{
    if (resource == "audio") resource = "sound";
    return resource + "." + action;
}

std::optional<std::string> TakeValue(
    const std::vector<std::string>& tokens,
    std::size_t& index,
    const std::string& option,
    std::string& error)
{
    if (index + 1 >= tokens.size())
    {
        error = "Missing value for " + option + ".";
        return std::nullopt;
    }
    ++index;
    return tokens[index];
}

#ifdef _WIN32
std::string WideToUtf8(const wchar_t* value)
{
    if (!value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}
#endif

} // namespace

CliParseResult ParseCliArguments(const std::vector<std::string>& arguments)
{
    CliParseResult result;
    if (arguments.empty())
    {
        result.errorMessage = "Executable path is missing.";
        return result;
    }

    CliInvocation invocation;
    invocation.runtime.configPath = ResolveDefaultConfigPath(
        GetExecutablePath(arguments.front()));

    std::vector<std::string> tokens(arguments.begin() + 1, arguments.end());
    if (!tokens.empty() && (tokens.front() == "--cli" || tokens.front() == "cli"))
        tokens.erase(tokens.begin());

    std::vector<std::string> commandTokens;
    std::optional<nlohmann::json> rawRequest;
    std::optional<CliInvocation::Mode> requestedMode;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        const std::string& token = tokens[index];
        std::string error;
        if (token == "--help" || token == "-h")
        {
            requestedMode = CliInvocation::Mode::Help;
            continue;
        }
        if (token == "--version")
        {
            requestedMode = CliInvocation::Mode::Version;
            continue;
        }
        if (token == "--schema")
        {
            requestedMode = CliInvocation::Mode::Schema;
            continue;
        }
        if (token == "--config")
        {
            const auto value = TakeValue(tokens, index, token, error);
            if (!value)
            {
                result.errorMessage = error;
                return result;
            }
            invocation.runtime.configPath = *value;
            invocation.runtime.loadConfig = true;
            continue;
        }
        if (token == "--no-config")
        {
            invocation.runtime.loadConfig = false;
            continue;
        }
        if (token == "--state-dir")
        {
            const auto value = TakeValue(tokens, index, token, error);
            if (!value || value->empty())
            {
                result.errorMessage = value
                    ? "--state-dir must not be empty."
                    : error;
                return result;
            }
            invocation.runtime.stateDirectory = *value;
            continue;
        }
        if (token == "--no-sound")
        {
            invocation.runtime.noSound = true;
            continue;
        }
        if (token == "--bluetooth")
        {
            invocation.runtime.startBluetooth = true;
            continue;
        }
        if (token == "--pretty")
        {
            invocation.pretty = true;
            continue;
        }
        if (token == "--quiet")
        {
            invocation.quiet = true;
            continue;
        }
        if (token == "--log-file")
        {
            const auto value = TakeValue(tokens, index, token, error);
            if (!value)
            {
                result.errorMessage = error;
                return result;
            }
            invocation.fileLogging = true;
            invocation.logFile = *value;
            continue;
        }
        if (token == "--log-level")
        {
            const auto value = TakeValue(tokens, index, token, error);
            if (!value)
            {
                result.errorMessage = error;
                return result;
            }
            invocation.logLevel = *value;
            continue;
        }
        if (token == "--wait")
        {
            const auto value = TakeValue(tokens, index, token, error);
            if (!value || !ParseDoubleStrict(*value, invocation.waitSeconds) ||
                invocation.waitSeconds < 0.0 || invocation.waitSeconds > 86400.0)
            {
                result.errorMessage = value
                    ? "--wait must be a finite number from 0 to 86400 seconds."
                    : error;
                return result;
            }
            continue;
        }
        if (token == "--tick-ms")
        {
            const auto value = TakeValue(tokens, index, token, error);
            if (!value || !ParseIntStrict(*value, invocation.tickMilliseconds) ||
                invocation.tickMilliseconds < 1 || invocation.tickMilliseconds > 1000)
            {
                result.errorMessage = value
                    ? "--tick-ms must be an integer from 1 to 1000."
                    : error;
                return result;
            }
            continue;
        }
        if (token == "--request")
        {
            const auto value = TakeValue(tokens, index, token, error);
            if (!value)
            {
                result.errorMessage = error;
                return result;
            }
            try
            {
                rawRequest = nlohmann::json::parse(*value);
            }
            catch (const std::exception& exception)
            {
                result.errorMessage = std::string("Invalid --request JSON: ") + exception.what();
                return result;
            }
            continue;
        }
        commandTokens.push_back(token);
    }

    if (requestedMode)
    {
        invocation.mode = *requestedMode;
        result.ok = true;
        result.invocation = std::move(invocation);
        return result;
    }
    if (rawRequest)
    {
        if (!commandTokens.empty())
        {
            result.errorMessage = "--request cannot be combined with a positional command.";
            return result;
        }
        if (!rawRequest->is_object() || !rawRequest->contains("command") ||
            !(*rawRequest)["command"].is_string())
        {
            result.errorMessage = "--request must contain a string field named 'command'.";
            return result;
        }
        invocation.mode = CliInvocation::Mode::Execute;
        invocation.command = (*rawRequest)["command"].get<std::string>();
        if (invocation.command.empty())
        {
            result.errorMessage = "--request field 'command' must not be empty.";
            return result;
        }
        if (rawRequest->contains("id"))
        {
            const auto& requestId = (*rawRequest)["id"];
            if (requestId.is_array() || requestId.is_object())
            {
                result.errorMessage =
                    "--request field 'id' must be a string, number, boolean, or null.";
                return result;
            }
            invocation.requestId = requestId;
        }
        invocation.parameters = rawRequest->value("params", nlohmann::json::object());
        if (!invocation.parameters.is_object())
        {
            result.errorMessage = "--request field 'params' must be an object.";
            return result;
        }
        result.ok = true;
        result.invocation = std::move(invocation);
        return result;
    }

    if (commandTokens.empty())
    {
        invocation.mode = CliInvocation::Mode::Help;
        result.ok = true;
        result.invocation = std::move(invocation);
        return result;
    }

    if (commandTokens.front() == "help")
    {
        invocation.mode = CliInvocation::Mode::Help;
        result.ok = true;
        result.invocation = std::move(invocation);
        return result;
    }
    if (commandTokens.front() == "version")
    {
        invocation.mode = CliInvocation::Mode::Version;
        result.ok = true;
        result.invocation = std::move(invocation);
        return result;
    }
    if (commandTokens.front() == "schema")
    {
        invocation.mode = CliInvocation::Mode::Schema;
        result.ok = true;
        result.invocation = std::move(invocation);
        return result;
    }
    if (commandTokens.front() == "serve")
    {
        if (commandTokens.size() != 1)
        {
            result.errorMessage = "serve does not accept positional arguments.";
            return result;
        }
        invocation.mode = CliInvocation::Mode::Serve;
        result.ok = true;
        result.invocation = std::move(invocation);
        return result;
    }

    std::string command;
    std::size_t argumentStart = 0;
    if (commandTokens.front() == "status")
    {
        command = "system.status";
        argumentStart = 1;
    }
    else if (commandTokens.front() == "health")
    {
        command = "system.health";
        argumentStart = 1;
    }
    else if (commandTokens.front() == "shutdown")
    {
        command = "system.shutdown";
        argumentStart = 1;
    }
    else if (commandTokens.front().find('.') != std::string::npos)
    {
        command = commandTokens.front();
        argumentStart = 1;
    }
    else
    {
        if (commandTokens.size() < 2 || StartsWith(commandTokens[1], "--"))
        {
            result.errorMessage = "A resource action is required (for example: playlist list).";
            return result;
        }
        command = CanonicalCommand(commandTokens[0], commandTokens[1]);
        argumentStart = 2;
    }

    const auto commandIt = CommandPositionals().find(command);
    if (commandIt == CommandPositionals().end())
    {
        result.errorMessage = "Unknown command '" + command + "'.";
        return result;
    }

    nlohmann::json parameters = nlohmann::json::object();
    std::vector<std::string> positionalValues;
    for (std::size_t index = argumentStart; index < commandTokens.size(); ++index)
    {
        const std::string& token = commandTokens[index];
        if (!StartsWith(token, "--"))
        {
            positionalValues.push_back(token);
            continue;
        }

        std::string option = token.substr(2);
        if (option.empty())
        {
            result.errorMessage = "Bare '--' is not supported; use named arguments for paths beginning with '--'.";
            return result;
        }

        bool value = true;
        std::optional<std::string> stringValue;
        const std::size_t equals = option.find('=');
        if (equals != std::string::npos)
        {
            stringValue = option.substr(equals + 1);
            option.resize(equals);
        }
        else if (StartsWith(option, "no-") || StartsWith(option, "no_"))
        {
            option = option.substr(3);
            value = false;
        }
        else if (!IsBooleanCommandOption(NormalizeKey(option)) &&
                 index + 1 < commandTokens.size() &&
                 !StartsWith(commandTokens[index + 1], "--"))
        {
            stringValue = commandTokens[++index];
        }

        const std::string key = NormalizeKey(option);
        if (key.empty() || parameters.contains(key))
        {
            result.errorMessage = key.empty()
                ? "Option name cannot be empty."
                : "Option '--" + option + "' was provided more than once.";
            return result;
        }
        parameters[key] = stringValue ? nlohmann::json(*stringValue) : nlohmann::json(value);
    }

    if (positionalValues.size() > commandIt->second.size())
    {
        result.errorMessage = "Too many positional arguments for '" + command + "'.";
        return result;
    }
    for (std::size_t index = 0; index < positionalValues.size(); ++index)
    {
        const std::string& key = commandIt->second[index];
        if (parameters.contains(key))
        {
            result.errorMessage = "Parameter '" + key + "' was provided twice.";
            return result;
        }
        parameters[key] = positionalValues[index];
    }

    invocation.mode = CliInvocation::Mode::Execute;
    invocation.command = command;
    invocation.parameters = std::move(parameters);
    result.ok = true;
    result.invocation = std::move(invocation);
    return result;
}

std::vector<std::string> GetUtf8CommandLineArguments(int argc, char** argv)
{
#ifdef _WIN32
    int wideCount = 0;
    wchar_t** wideArguments = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (wideArguments)
    {
        std::vector<std::string> result;
        result.reserve(static_cast<std::size_t>(wideCount));
        for (int index = 0; index < wideCount; ++index)
            result.push_back(WideToUtf8(wideArguments[index]));
        LocalFree(wideArguments);
        return result;
    }
#endif
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
        result.emplace_back(argv[index] ? argv[index] : "");
    return result;
}

bool IsCliInvocation(const std::vector<std::string>& arguments)
{
    if (arguments.size() < 2) return false;
    const std::string& first = arguments[1];
    return first == "--cli" || first == "cli" || first == "--help" || first == "-h" ||
           first == "--version" || first == "--schema";
}

} // namespace TSM
