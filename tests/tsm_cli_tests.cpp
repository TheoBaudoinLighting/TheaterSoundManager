#include "pch.h"

#include <cstdint>
#include <thread>

namespace TSM::Tests
{
namespace
{

std::filesystem::path UniqueTemporaryPath(const char* suffix)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("tsm_cli_" + std::to_string(stamp) + suffix);
}

void WriteLittleEndian16(std::ofstream& output, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu)
    };
    output.write(bytes, sizeof(bytes));
}

void WriteLittleEndian32(std::ofstream& output, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8u) & 0xffu),
        static_cast<char>((value >> 16u) & 0xffu),
        static_cast<char>((value >> 24u) & 0xffu)
    };
    output.write(bytes, sizeof(bytes));
}

void WriteSilentWave(const std::filesystem::path& path)
{
    constexpr std::uint32_t sampleRate = 8000;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bitsPerSample = 16;
    constexpr std::uint32_t sampleCount = 800;
    constexpr std::uint32_t dataSize = sampleCount * channels * (bitsPerSample / 8u);

    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.is_open());
    output.write("RIFF", 4);
    WriteLittleEndian32(output, 36u + dataSize);
    output.write("WAVEfmt ", 8);
    WriteLittleEndian32(output, 16);
    WriteLittleEndian16(output, 1);
    WriteLittleEndian16(output, channels);
    WriteLittleEndian32(output, sampleRate);
    WriteLittleEndian32(output, sampleRate * channels * (bitsPerSample / 8u));
    WriteLittleEndian16(output, channels * (bitsPerSample / 8u));
    WriteLittleEndian16(output, bitsPerSample);
    output.write("data", 4);
    WriteLittleEndian32(output, dataSize);
    const std::vector<char> silence(dataSize, 0);
    output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
}

std::string NormalizedCacheKey(const std::filesystem::path& path)
{
    const std::u8string normalized =
        std::filesystem::absolute(path).lexically_normal().generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(normalized.data()), normalized.size());
}

nlohmann::json ValidLoudnessCacheEntry(
    const std::filesystem::path& path, float integratedLufs, float truePeakDb)
{
    return {
        {"size", std::filesystem::file_size(path)},
        {"writeTime", static_cast<std::int64_t>(
            std::filesystem::last_write_time(path).time_since_epoch().count())},
        {"integratedLufs", integratedLufs},
        {"truePeakDb", truePeakDb}
    };
}

void WriteJson(const std::filesystem::path& path, const nlohmann::json& value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output << value.dump(2);
    ASSERT_TRUE(output.good());
}

bool WaitForLoudnessStatus(
    ApplicationRuntime& runtime, const std::string& soundName,
    AudioManager::LoudnessStatus expectedStatus)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        runtime.Tick(0.01f);
        const auto& sounds = AudioManager::GetInstance().GetAllSounds();
        const auto sound = sounds.find(soundName);
        if (sound != sounds.end() && sound->second.loudnessStatus == expectedStatus)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool WaitForLoudnessTerminalState(
    ApplicationRuntime& runtime, const std::string& soundName)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        runtime.Tick(0.01f);
        const auto& sounds = AudioManager::GetInstance().GetAllSounds();
        const auto sound = sounds.find(soundName);
        if (sound != sounds.end() &&
            sound->second.loudnessStatus != AudioManager::LoudnessStatus::Queued &&
            sound->second.loudnessStatus != AudioManager::LoudnessStatus::Analyzing)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

class CliRuntimeTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        stateDirectory = UniqueTemporaryPath("_state");
        ASSERT_TRUE(std::filesystem::create_directories(stateDirectory));
        temporaryPaths.push_back(stateDirectory);
        RuntimeOptions options;
        options.loadConfig = false;
        options.noSound = true;
        options.stateDirectory = stateDirectory.string();
        ASSERT_TRUE(runtime.Initialize(options, error)) << error;
    }

    void TearDown() override
    {
        runtime.Shutdown();
        for (const auto& temporaryPath : temporaryPaths)
        {
            std::error_code removeError;
            if (std::filesystem::is_directory(temporaryPath, removeError))
                std::filesystem::remove_all(temporaryPath, removeError);
            else
                std::filesystem::remove(temporaryPath, removeError);
        }
    }

    std::filesystem::path CreateWave()
    {
        const std::filesystem::path path = CreateTemporaryPath(".wav");
        WriteSilentWave(path);
        return path;
    }

    std::filesystem::path CreateTemporaryPath(const char* suffix)
    {
        const std::filesystem::path path = UniqueTemporaryPath(suffix);
        temporaryPaths.push_back(path);
        return path;
    }

    ApplicationRuntime runtime;
    std::string error;
    std::vector<std::filesystem::path> temporaryPaths;
    std::filesystem::path stateDirectory;
};

} // namespace

TEST(CliParserTests, ParsesCommandGlobalOptionsAndTypedFlags)
{
    const CliParseResult parsed = ParseCliArguments({
        "TheaterSoundManager.exe",
        "--cli",
        "playlist",
        "play",
        "Evening Set",
        "--random-order",
        "--segment-duration",
        "42.5",
        "--no-loop",
        "--config",
        "C:/Show/config.json",
        "--no-sound"
    });

    ASSERT_TRUE(parsed.ok) << parsed.errorMessage;
    EXPECT_EQ(parsed.invocation.mode, CliInvocation::Mode::Execute);
    EXPECT_EQ(parsed.invocation.command, "playlist.play");
    EXPECT_EQ(parsed.invocation.parameters.at("name"), "Evening Set");
    EXPECT_EQ(parsed.invocation.parameters.at("random_order"), true);
    EXPECT_EQ(parsed.invocation.parameters.at("segment_duration"), "42.5");
    EXPECT_EQ(parsed.invocation.parameters.at("loop"), false);
    EXPECT_EQ(parsed.invocation.runtime.configPath, "C:/Show/config.json");
    EXPECT_TRUE(parsed.invocation.runtime.noSound);
}

TEST(CliParserTests, ParsesGlobalMusicLibraryPlaybackOptions)
{
    const CliParseResult parsed = ParseCliArguments({
        "TheaterSoundManager.exe",
        "--cli",
        "library",
        "play",
        "--random-order",
        "--no-random-segment",
        "--crossfade",
        "3.5",
        "--no-loop"
    });

    ASSERT_TRUE(parsed.ok) << parsed.errorMessage;
    EXPECT_EQ(parsed.invocation.mode, CliInvocation::Mode::Execute);
    EXPECT_EQ(parsed.invocation.command, "library.play");
    EXPECT_EQ(parsed.invocation.parameters.at("random_order"), true);
    EXPECT_EQ(parsed.invocation.parameters.at("random_segment"), false);
    EXPECT_EQ(parsed.invocation.parameters.at("crossfade"), "3.5");
    EXPECT_EQ(parsed.invocation.parameters.at("loop"), false);
}

TEST(CliParserTests, PreservesUnicodeAndParsesRawRequest)
{
    const CliParseResult parsed = ParseCliArguments({
        "TheaterSoundManager.exe",
        "--cli",
        "--request",
        R"({"id":"écho","command":"sound.load","params":{"id":"entrée","path":"C:/Média/été.wav"}})"
    });

    ASSERT_TRUE(parsed.ok) << parsed.errorMessage;
    EXPECT_EQ(parsed.invocation.command, "sound.load");
    EXPECT_EQ(parsed.invocation.requestId, "écho");
    EXPECT_EQ(parsed.invocation.parameters.at("id"), "entrée");
    EXPECT_EQ(parsed.invocation.parameters.at("path"), "C:/Média/été.wav");
}

TEST(CliParserTests, BooleanFlagDoesNotConsumeFollowingPositional)
{
    const CliParseResult parsed = ParseCliArguments({
        "TheaterSoundManager.exe", "--cli", "sound", "play", "--loop", "track"
    });
    ASSERT_TRUE(parsed.ok) << parsed.errorMessage;
    EXPECT_EQ(parsed.invocation.parameters.at("id"), "track");
    EXPECT_EQ(parsed.invocation.parameters.at("loop"), true);
}

TEST(CliParserTests, ParsesCinemaSafetyCommandsAndStateDirectory)
{
    const CliParseResult heartbeat = ParseCliArguments({
        "TheaterSoundManager.exe", "--cli", "safety", "heartbeat",
        "fire-panel", "boot-42", "7", "safe",
        "--state-dir", "C:/Cinema/TSM-state"
    });
    ASSERT_TRUE(heartbeat.ok) << heartbeat.errorMessage;
    EXPECT_EQ(heartbeat.invocation.command, "safety.heartbeat");
    EXPECT_EQ(heartbeat.invocation.parameters.at("source_id"), "fire-panel");
    EXPECT_EQ(heartbeat.invocation.parameters.at("session_id"), "boot-42");
    EXPECT_EQ(heartbeat.invocation.parameters.at("sequence"), "7");
    EXPECT_EQ(heartbeat.invocation.parameters.at("state"), "safe");
    EXPECT_EQ(
        heartbeat.invocation.runtime.stateDirectory, "C:/Cinema/TSM-state");

    const CliParseResult trip = ParseCliArguments({
        "TheaterSoundManager.exe", "--cli", "safety.trip",
        "manual_emergency", "operator-console", "--play-evacuation"
    });
    ASSERT_TRUE(trip.ok) << trip.errorMessage;
    EXPECT_TRUE(trip.invocation.parameters.at("play_evacuation"));
}

TEST(CliParserTests, RejectsUnknownCommandAndDuplicateParameters)
{
    const CliParseResult unknown = ParseCliArguments({
        "TheaterSoundManager.exe", "--cli", "playlist", "unknown"
    });
    EXPECT_FALSE(unknown.ok);
    EXPECT_NE(unknown.errorMessage.find("Unknown command"), std::string::npos);

    const CliParseResult duplicate = ParseCliArguments({
        "TheaterSoundManager.exe", "--cli", "sound", "show", "track", "--id", "other"
    });
    EXPECT_FALSE(duplicate.ok);
    EXPECT_NE(duplicate.errorMessage.find("provided twice"), std::string::npos);
}

TEST(CliContractTests, BuildsStableSuccessAndErrorEnvelopes)
{
    const nlohmann::json success = BuildCliResponse(
        CliResult::Success({{"pong", true}}), "system.ping", "request-1");
    EXPECT_EQ(success.at("schemaVersion"), 1);
    EXPECT_EQ(success.at("apiVersion"), "1.0");
    EXPECT_EQ(success.at("id"), "request-1");
    EXPECT_TRUE(success.at("ok"));
    EXPECT_TRUE(success.at("data").at("pong"));
    EXPECT_TRUE(success.at("error").is_null());

    const nlohmann::json failure = BuildCliResponse(
        CliResult::Failure(CliExitCode::NotFound, "missing", "Not found."),
        "sound.show",
        42);
    EXPECT_FALSE(failure.at("ok"));
    EXPECT_TRUE(failure.at("data").is_null());
    EXPECT_EQ(failure.at("error").at("code"), "missing");
    EXPECT_EQ(failure.at("id"), 42);
}

TEST(CliContractTests, HelpPublishesGlobalMusicLibraryCommands)
{
    const std::string help = GetCliHelp();
    EXPECT_NE(help.find("library       play, stop, next, status"), std::string::npos);
    EXPECT_NE(help.find("--cli library play"), std::string::npos);
}

TEST(CliContractTests, SchemaDescribesBooleanIdsAndStableExitCodes)
{
    const nlohmann::json schema = CliCommandProcessor::GetSchema();
    EXPECT_NE(schema.at("request").at("id").get<std::string>().find("boolean"),
              std::string::npos);
    EXPECT_EQ(schema.at("exitCodes").at("inputOutput"), 8);
    EXPECT_EQ(schema.at("limits").at("requestBytes"), 1024 * 1024);
}

TEST(CliContractTests, EveryPublishedCommandHasAConsistentParameterSchema)
{
    const nlohmann::json schema = CliCommandProcessor::GetSchema();
    EXPECT_EQ(
        schema.at("parameterSchemaDialect"),
        "https://json-schema.org/draft/2020-12/schema");

    const auto& commands = schema.at("commands");
    const std::vector<std::string> expectedCurrentCommands = {
        "system.ping", "system.status", "system.health", "system.capabilities",
        "system.shutdown", "config.validate", "config.show", "config.reload",
        "cinema.status", "cinema.schedules", "cinema.preview",
        "safety.status", "safety.heartbeat", "safety.trip", "safety.reset",
        "session.status", "session.resume", "session.discard",
        "sound.list", "sound.show", "sound.load", "sound.unload", "sound.play",
        "sound.stop", "sound.stop-all", "sound.set", "sound.pause", "sound.resume",
        "sound.seek", "playlist.list", "playlist.show", "playlist.create",
        "playlist.delete", "playlist.rename", "playlist.duplicate", "playlist.add",
        "playlist.remove", "playlist.clear", "playlist.move", "playlist.options",
        "playlist.import", "playlist.export", "playlist.save", "playlist.load",
        "playlist.play", "playlist.play-index", "playlist.stop", "playlist.next",
        "playlist.status", "library.play", "library.stop", "library.next",
        "library.status", "announcement.list", "announcement.load",
        "announcement.unload", "announcement.play", "announcement.stop",
        "announcement.status", "schedule.list", "schedule.add", "schedule.update",
        "schedule.remove", "schedule.reset", "mixer.get", "mixer.set",
        "wedding.status", "wedding.asset", "wedding.phase", "wedding.next",
        "wedding.stop", "loudness.status", "loudness.analyze", "loudness.target",
        "loudness.clear-cache"
    };
    EXPECT_GE(commands.size(), expectedCurrentCommands.size());

    std::vector<std::string> publishedNames;
    for (const auto& entry : commands)
    {
        ASSERT_TRUE(entry.contains("name") && entry.at("name").is_string());
        publishedNames.push_back(entry.at("name").get<std::string>());

        ASSERT_TRUE(entry.at("positionals").is_array()) << entry.at("name");
        const auto& parameterSchema = entry.at("paramsSchema");
        EXPECT_EQ(parameterSchema.at("type"), "object") << entry.at("name");
        EXPECT_EQ(parameterSchema.at("additionalProperties"), false) << entry.at("name");
        ASSERT_TRUE(parameterSchema.at("properties").is_object()) << entry.at("name");
        ASSERT_TRUE(parameterSchema.at("required").is_array()) << entry.at("name");
        for (const auto& required : parameterSchema.at("required"))
        {
            ASSERT_TRUE(required.is_string()) << entry.at("name");
            EXPECT_TRUE(parameterSchema.at("properties").contains(
                required.get<std::string>())) << entry.at("name") << required;
        }
        for (const auto& positional : entry.at("positionals"))
        {
            ASSERT_TRUE(positional.is_string()) << entry.at("name");
            EXPECT_TRUE(parameterSchema.at("properties").contains(
                positional.get<std::string>())) << entry.at("name") << positional;
        }
    }

    std::vector<std::string> sortedNames = publishedNames;
    std::sort(sortedNames.begin(), sortedNames.end());
    EXPECT_EQ(
        std::adjacent_find(sortedNames.begin(), sortedNames.end()),
        sortedNames.end()) << "The CLI schema contains duplicate command names.";
    for (const std::string& expected : expectedCurrentCommands)
    {
        EXPECT_NE(
            std::find(publishedNames.begin(), publishedNames.end(), expected),
            publishedNames.end()) << "Missing command schema: " << expected;
    }
}

TEST(CliContractTests, ParameterSchemasExposeTypesDefaultsRangesAndConstraints)
{
    const nlohmann::json schema = CliCommandProcessor::GetSchema();
    const auto& commands = schema.at("commands");
    const auto findCommand = [&commands](const char* name) {
        return std::find_if(commands.begin(), commands.end(), [name](const auto& entry) {
            return entry.at("name") == name;
        });
    };

    const auto soundPlay = findCommand("sound.play");
    ASSERT_NE(soundPlay, commands.end());
    const auto& soundPlaySchema = soundPlay->at("paramsSchema");
    EXPECT_NE(
        std::find(soundPlaySchema.at("required").begin(),
                  soundPlaySchema.at("required").end(), "id"),
        soundPlaySchema.at("required").end());
    const auto& volume = soundPlaySchema.at("properties").at("volume");
    EXPECT_EQ(volume.at("type"), "number");
    EXPECT_EQ(volume.at("minimum"), 0.0);
    EXPECT_EQ(volume.at("maximum"), 3.0);
    EXPECT_EQ(volume.at("default"), 1.0);
    EXPECT_EQ(soundPlaySchema.at("properties").at("loop").at("default"), false);

    const auto soundLoad = findCommand("sound.load");
    ASSERT_NE(soundLoad, commands.end());
    const auto& soundLoadProperties = soundLoad->at("paramsSchema").at("properties");
    EXPECT_EQ(soundLoadProperties.at("kind").at("default"), "sfx");
    EXPECT_NE(
        std::find(soundLoadProperties.at("kind").at("enum").begin(),
                  soundLoadProperties.at("kind").at("enum").end(), "music"),
        soundLoadProperties.at("kind").at("enum").end());
    EXPECT_EQ(
        soundLoadProperties.at("stream").at("x-tsm-defaultBy").at("parameter"),
        "kind");

    const auto soundSet = findCommand("sound.set");
    ASSERT_NE(soundSet, commands.end());
    EXPECT_EQ(soundSet->at("paramsSchema").at("anyOf").size(), 2u);

    const auto playlistRemove = findCommand("playlist.remove");
    ASSERT_NE(playlistRemove, commands.end());
    EXPECT_EQ(playlistRemove->at("paramsSchema").at("oneOf").size(), 2u);

    const auto libraryPlay = findCommand("library.play");
    ASSERT_NE(libraryPlay, commands.end());
    EXPECT_TRUE(libraryPlay->at("positionals").empty());
    EXPECT_TRUE(libraryPlay->at("paramsSchema").at("required").empty());
    const auto& libraryPlayProperties =
        libraryPlay->at("paramsSchema").at("properties");
    EXPECT_EQ(
        libraryPlayProperties.at("random_order").at("x-tsm-omitted"),
        "preserveLibrary");
    EXPECT_EQ(libraryPlayProperties.at("crossfade").at("minimum"), 0.0);
    EXPECT_EQ(libraryPlayProperties.at("crossfade").at("maximum"), 3600.0);

    const auto scheduleAdd = findCommand("schedule.add");
    ASSERT_NE(scheduleAdd, commands.end());
    EXPECT_EQ(scheduleAdd->at("paramsSchema").at("oneOf").size(), 2u);
    EXPECT_EQ(
        scheduleAdd->at("paramsSchema").at("properties").at("hour").at("maximum"),
        23);

    const auto mixerSet = findCommand("mixer.set");
    ASSERT_NE(mixerSet, commands.end());
    EXPECT_EQ(mixerSet->at("paramsSchema").at("anyOf").size(), 5u);

    const auto loudnessTarget = findCommand("loudness.target");
    ASSERT_NE(loudnessTarget, commands.end());
    const auto& target = loudnessTarget->at("paramsSchema").at("properties").at("value");
    EXPECT_EQ(target.at("minimum"), -30.0);
    EXPECT_EQ(target.at("maximum"), -8.0);
}

TEST(CliConfigTests, ReportsAllSemanticValidationErrors)
{
    const std::filesystem::path path = UniqueTemporaryPath(".json");
    {
        std::ofstream output(path);
        output << R"({
            "loudnessTargetLufs": 3,
            "playlists": [
                {"name":"duplicate","tracks":[]},
                {"name":"duplicate","options":{"segmentDuration":0},"tracks":[{"id":"","path":""}]}
            ],
            "announcements": [{"id":"announcement","path":"a.wav","hour":18446744073709551615,"minute":0}]
        })";
    }

    std::vector<ConfigValidationIssue> issues;
    std::string validationError;
    EXPECT_FALSE(ValidateAppConfig(path.string(), issues, validationError));
    EXPECT_TRUE(validationError.empty());
    EXPECT_GE(issues.size(), 5u);

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

TEST(CliCommandTests, PingWorksWithoutAudioRuntime)
{
    ApplicationRuntime runtime;
    CliCommandProcessor processor(runtime);
    const CliResult result = processor.Execute("system.ping", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_TRUE(result.data.at("pong"));
}

TEST_F(CliRuntimeTests, MixerAndPlaylistCrudAreExposed)
{
    CliCommandProcessor processor(runtime);
    CliResult result = processor.Execute(
        "mixer.set", {{"master", 0.8}, {"music", 0.6}, {"duck", 0.5}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_FLOAT_EQ(MixerState::GetInstance().GetMasterVolume(), 0.8f);
    EXPECT_FLOAT_EQ(MixerState::GetInstance().GetMusicVolume(), 0.6f);
    EXPECT_FLOAT_EQ(MixerState::GetInstance().GetDuckFactor(), 0.5f);

    result = processor.Execute("playlist.create", {{"name", "CLI playlist"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute(
        "playlist.options",
        {{"name", "CLI playlist"}, {"random_order", true}, {"segment_duration", 25.0}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_TRUE(result.data.at("playlist").at("options").at("randomOrder"));
    EXPECT_FLOAT_EQ(
        result.data.at("playlist").at("options").at("segmentDuration").get<float>(), 25.0f);

    result = processor.Execute("playlist.delete", {{"name", "CLI playlist"}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
}

TEST_F(CliRuntimeTests, GlobalMusicLibraryIncludesPlaylistAndOrphanMusicOnly)
{
    CliCommandProcessor processor(runtime);

    CliResult result = processor.Execute("library.play", nlohmann::json::object());
    EXPECT_EQ(result.exitCode, CliExitCode::Conflict);
    EXPECT_EQ(result.errorCode, "music_library_not_playable");

    result = processor.Execute("library.next", nlohmann::json::object());
    EXPECT_EQ(result.exitCode, CliExitCode::Conflict);
    EXPECT_EQ(result.errorCode, "music_library_not_playing");

    const std::filesystem::path wave = CreateWave();
    const std::vector<std::pair<std::string, std::string>> sounds = {
        {"music_in_playlist", "music"},
        {"music_orphan", "music"},
        {"announcement", "announcement"},
        {"wedding", "wedding"},
        {"sound_effect", "sfx"}
    };
    for (const auto& [id, kind] : sounds)
    {
        result = processor.Execute(
            "sound.load", {{"id", id}, {"path", wave.string()}, {"kind", kind}});
        ASSERT_TRUE(result.IsSuccess()) << id << ": " << result.errorMessage;
    }

    for (const char* playlistName : {"pre_show", "post_show"})
    {
        result = processor.Execute("playlist.create", {{"name", playlistName}});
        ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
        result = processor.Execute(
            "playlist.add", {{"name", playlistName}, {"id", "music_in_playlist"}});
        ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    }

    result = processor.Execute("library.status", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    const auto& initialLibrary = result.data.at("library");
    EXPECT_EQ(initialLibrary.at("source"), "music_library");
    EXPECT_FALSE(initialLibrary.at("playing"));
    EXPECT_EQ(initialLibrary.at("trackCount"), 2u);
    EXPECT_EQ(
        initialLibrary.at("tracks").get<std::vector<std::string>>(),
        (std::vector<std::string>{"music_in_playlist", "music_orphan"}));

    result = processor.Execute(
        "playlist.play",
        {{"name", "pre_show"}, {"random_order", false},
         {"random_segment", false}, {"loop", true}, {"crossfade", 0.0}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;

    result = processor.Execute(
        "library.play",
        {{"random_order", false}, {"random_segment", false},
         {"loop", true}, {"crossfade", 2.5}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    const auto& playingLibrary = result.data.at("library");
    EXPECT_TRUE(playingLibrary.at("playing"));
    EXPECT_EQ(playingLibrary.at("trackCount"), 2u);
    EXPECT_EQ(playingLibrary.at("availableTrackCount"), 2u);
    EXPECT_FALSE(playingLibrary.at("options").at("randomOrder"));
    EXPECT_FALSE(playingLibrary.at("options").at("randomSegment"));
    EXPECT_TRUE(playingLibrary.at("options").at("loop"));
    EXPECT_FLOAT_EQ(
        playingLibrary.at("options").at("crossfadeDuration").get<float>(), 2.5f);

    result = processor.Execute("playlist.status", {{"name", "pre_show"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_FALSE(result.data.at("active").at("playing"));

    result = processor.Execute("system.status", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_TRUE(result.data.at("activePlaylist").is_null());
    EXPECT_TRUE(result.data.at("musicLibrary").at("playing"));
    EXPECT_EQ(result.data.at("musicLibrary").at("trackCount"), 2u);

    result = processor.Execute(
        "sound.load",
        {{"id", "music_loaded_later"}, {"path", wave.string()}, {"kind", "music"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute("library.status", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_EQ(result.data.at("library").at("trackCount"), 2u);
    EXPECT_EQ(result.data.at("library").at("availableTrackCount"), 3u);
    EXPECT_EQ(
        result.data.at("library").at("tracks").get<std::vector<std::string>>(),
        (std::vector<std::string>{"music_in_playlist", "music_orphan"}));

    result = processor.Execute("sound.unload", {{"id", "music_orphan"}});
    EXPECT_EQ(result.exitCode, CliExitCode::Conflict);
    EXPECT_EQ(result.errorCode, "sound_in_use");
    EXPECT_TRUE(
        result.errorDetails.at("references").at("activeMusicLibrary"));

    result = processor.Execute("library.next", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_TRUE(result.data.at("library").at("playing"));

    result = processor.Execute("library.stop", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_FALSE(result.data.at("library").at("playing"));

    runtime.Tick(2.0f);
    result = processor.Execute("sound.unload", {{"id", "music_orphan"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;

    result = processor.Execute("library.status", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_EQ(result.data.at("library").at("trackCount"), 2u);
    EXPECT_EQ(result.data.at("library").at("availableTrackCount"), 2u);
}

TEST_F(CliRuntimeTests, MutationsValidateCompletelyBeforeChangingState)
{
    CliCommandProcessor processor(runtime);
    MixerState::GetInstance().Reset();
    const float originalMaster = MixerState::GetInstance().GetMasterVolume();
    CliResult result = processor.Execute(
        "mixer.set", {{"master", 0.8}, {"music", 99.0}});
    EXPECT_FALSE(result.IsSuccess());
    EXPECT_EQ(result.exitCode, CliExitCode::InvalidArgument);
    EXPECT_FLOAT_EQ(MixerState::GetInstance().GetMasterVolume(), originalMaster);

    result = processor.Execute(
        "wedding.phase", {{"phase", std::uint64_t{4294967297ULL}}});
    EXPECT_FALSE(result.IsSuccess());
    EXPECT_EQ(result.exitCode, CliExitCode::InvalidArgument);

    result = processor.Execute("playlist.create", {{"name", "atomic"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute(
        "playlist.options",
        {{"name", "atomic"}, {"random_order", false}, {"segment_duration", -1}});
    EXPECT_FALSE(result.IsSuccess());
    const auto* playlist = PlaylistManager::GetInstance().GetPlaylistByName("atomic");
    ASSERT_NE(playlist, nullptr);
    EXPECT_TRUE(playlist->options.randomOrder);
}

TEST_F(CliRuntimeTests, MixerBusPreservesPerChannelGain)
{
    const std::filesystem::path wave = CreateWave();
    CliCommandProcessor processor(runtime);
    CliResult result = processor.Execute(
        "sound.load",
        {{"id", "music"}, {"path", wave.string()}, {"kind", "music"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute(
        "mixer.set", {{"master", 0.8}, {"music", 0.5}, {"duck", 0.5}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute(
        "sound.play", {{"id", "music"}, {"volume", 0.4}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;

    FMOD::Channel* channel = AudioManager::GetInstance().GetLastChannelOfSound("music");
    ASSERT_NE(channel, nullptr);
    float localVolume = 0.0f;
    ASSERT_EQ(channel->getVolume(&localVolume), FMOD_OK);
    EXPECT_NEAR(localVolume, 0.4f, 0.001f);
    FMOD::ChannelGroup* group = nullptr;
    ASSERT_EQ(channel->getChannelGroup(&group), FMOD_OK);
    ASSERT_NE(group, nullptr);
    float groupVolume = 0.0f;
    ASSERT_EQ(group->getVolume(&groupVolume), FMOD_OK);
    EXPECT_NEAR(groupVolume, 0.2f, 0.001f);

    result = processor.Execute(
        "mixer.set", {{"master", 1.0}, {"music", 0.25}, {"duck", 1.0}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    ASSERT_EQ(channel->getVolume(&localVolume), FMOD_OK);
    EXPECT_NEAR(localVolume, 0.4f, 0.001f);
    ASSERT_EQ(group->getVolume(&groupVolume), FMOD_OK);
    EXPECT_NEAR(groupVolume, 0.25f, 0.001f);
}

TEST_F(CliRuntimeTests, UnloadRejectsReferencedAndMismatchedResources)
{
    const std::filesystem::path wave = CreateWave();
    CliCommandProcessor processor(runtime);
    CliResult result = processor.Execute(
        "sound.load",
        {{"id", "track"}, {"path", wave.string()}, {"kind", "music"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute("playlist.create", {{"name", "referencing"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute(
        "playlist.add", {{"name", "referencing"}, {"id", "track"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;

    result = processor.Execute("sound.unload", {{"id", "track"}});
    EXPECT_EQ(result.exitCode, CliExitCode::Conflict);
    EXPECT_EQ(result.errorCode, "sound_in_use");
    result = processor.Execute("announcement.unload", {{"id", "track"}});
    EXPECT_EQ(result.exitCode, CliExitCode::NotFound);

    result = processor.Execute(
        "playlist.remove", {{"name", "referencing"}, {"id", "track"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute("sound.unload", {{"id", "track"}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
}

TEST_F(CliRuntimeTests, DuckLayersComposeAndWeddingAutomationInterpolates)
{
    auto& mixer = MixerState::GetInstance();
    mixer.SetDuckFactor(0.8f);
    mixer.SetWeddingDuckFactor(0.5f);
    mixer.SetAnnouncementDuckFactor(0.25f);
    EXPECT_NEAR(mixer.GetEffectiveDuckFactor(), 0.1f, 0.0001f);

    mixer.SetDuckFactor(1.0f);
    mixer.SetWeddingDuckFactor(1.0f);
    mixer.SetAnnouncementDuckFactor(1.0f);
    const std::filesystem::path wave = CreateWave();
    ASSERT_TRUE(AudioManager::GetInstance().LoadWeddingPhaseSound(2, wave.string()));
    auto& ui = UIManager::GetInstance();
    ui.StartWeddingPhase2(false);
    runtime.Tick(1.0f);
    EXPECT_GT(mixer.GetWeddingDuckFactor(), 0.0f);
    EXPECT_LT(mixer.GetWeddingDuckFactor(), 0.05f);
    ui.StopWeddingMode();

    ASSERT_TRUE(AudioManager::GetInstance().LoadWeddingPhaseSound(3, wave.string()));
    ui.StartWeddingPhase3(false);
    runtime.Tick(1.0f);
    EXPECT_GT(mixer.GetWeddingDuckFactor(), 0.05f);
    EXPECT_LT(mixer.GetWeddingDuckFactor(), 0.2f);
    ui.StopWeddingMode();
}

TEST_F(CliRuntimeTests, WeddingPhaseStopsAnnouncementBeforeGlobalAudioReset)
{
    const std::filesystem::path wave = CreateWave();
    auto& audio = AudioManager::GetInstance();
    auto& announcements = AnnouncementManager::GetInstance();
    auto& mixer = MixerState::GetInstance();

    ASSERT_TRUE(audio.LoadAnnouncement("ceremony_notice", wave.string()));
    ASSERT_TRUE(audio.LoadWeddingPhaseSound(2, wave.string()));
    announcements.PlayAnnouncement("ceremony_notice", 0.25f, false, false);
    runtime.Tick(0.5f);
    ASSERT_TRUE(announcements.IsAnnouncing());
    ASSERT_LT(mixer.GetAnnouncementDuckFactor(), 1.0f);

    auto& ui = UIManager::GetInstance();
    ui.StartWeddingPhase2(false);

    EXPECT_FALSE(announcements.IsAnnouncing());
    EXPECT_EQ(announcements.GetAnnouncementState(), AnnouncementState::IDLE);
    EXPECT_TRUE(announcements.GetCurrentAnnouncementName().empty());
    EXPECT_FLOAT_EQ(mixer.GetAnnouncementDuckFactor(), 1.0f);
    EXPECT_TRUE(ui.IsWeddingModeActive());
    ui.StopWeddingMode();
}

TEST_F(CliRuntimeTests, InvalidWeddingAssetReplacementPreservesPreviousAsset)
{
    const std::filesystem::path wave = CreateWave();
    CliCommandProcessor processor(runtime);
    CliResult result = processor.Execute(
        "wedding.asset", {{"phase", 1}, {"path", wave.string()}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;

    auto& audio = AudioManager::GetInstance();
    const auto before = audio.GetAllSounds().find("wedding_entrance_sound");
    ASSERT_NE(before, audio.GetAllSounds().end());
    FMOD::Sound* const previousSound = before->second.sound;
    const std::string previousPath = before->second.filePath;

    const std::filesystem::path missing = UniqueTemporaryPath("_missing.wav");
    ASSERT_FALSE(std::filesystem::exists(missing));
    result = processor.Execute(
        "wedding.asset", {{"phase", 1}, {"path", missing.string()}});
    EXPECT_EQ(result.exitCode, CliExitCode::AudioEngine);
    EXPECT_EQ(result.errorCode, "wedding_asset_load_failed");

    const auto after = audio.GetAllSounds().find("wedding_entrance_sound");
    ASSERT_NE(after, audio.GetAllSounds().end());
    EXPECT_EQ(after->second.sound, previousSound);
    EXPECT_EQ(after->second.filePath, previousPath);
    EXPECT_EQ(after->second.kind, AudioManager::SoundKind::Wedding);
}

TEST_F(CliRuntimeTests, MalformedLoudnessCachesAreIgnoredWithoutStoppingTheWorker)
{
    const std::filesystem::path wave = CreateWave();
    const std::filesystem::path cachePath = CreateTemporaryPath("_loudness.json");
    const std::string cacheKey = NormalizedCacheKey(wave);
    auto& audioManager = AudioManager::GetInstance();
    ASSERT_TRUE(audioManager.LoadSound(
        "malformed_cache_music", wave.string(), true, AudioManager::SoundKind::Music));

    std::vector<nlohmann::json> malformedCaches;
    malformedCaches.push_back(nullptr);
    malformedCaches.push_back(nlohmann::json::array());
    nlohmann::json primitiveEntry = nlohmann::json::object();
    primitiveEntry[cacheKey] = nlohmann::json::array();
    malformedCaches.push_back(std::move(primitiveEntry));
    nlohmann::json invalidFields = nlohmann::json::object();
    invalidFields[cacheKey] = {
        {"size", "not-a-size"},
        {"writeTime", nullptr},
        {"integratedLufs", nlohmann::json::array()},
        {"truePeakDb", false}
    };
    malformedCaches.push_back(std::move(invalidFields));

    for (const nlohmann::json& malformedCache : malformedCaches)
    {
        audioManager.ConfigureLoudness(cachePath.string(), -16.0f);
        WriteJson(cachePath, malformedCache);
        audioManager.QueueLoudnessAnalysis("malformed_cache_music");
        ASSERT_TRUE(WaitForLoudnessTerminalState(runtime, "malformed_cache_music"));

        bool removed = false;
        std::string clearError;
        ASSERT_TRUE(audioManager.ClearLoudnessCache(removed, clearError)) << clearError;
        EXPECT_TRUE(removed);
        EXPECT_EQ(
            audioManager.GetAllSounds().at("malformed_cache_music").loudnessStatus,
            AudioManager::LoudnessStatus::NotQueued);
    }
}

TEST_F(CliRuntimeTests, ClearLoudnessCacheInvalidatesReadyGainAndAllowsReanalysis)
{
    const std::filesystem::path wave = CreateWave();
    const std::filesystem::path cachePath = CreateTemporaryPath("_loudness.json");
    const std::string cacheKey = NormalizedCacheKey(wave);
    auto& audioManager = AudioManager::GetInstance();
    ASSERT_TRUE(audioManager.LoadSound(
        "cached_music", wave.string(), true, AudioManager::SoundKind::Music));
    audioManager.ConfigureLoudness(cachePath.string(), -16.0f);

    nlohmann::json cache = nlohmann::json::object();
    cache[cacheKey] = ValidLoudnessCacheEntry(wave, -20.0f, -6.0f);
    WriteJson(cachePath, cache);
    audioManager.QueueLoudnessAnalysis("cached_music");
    ASSERT_TRUE(WaitForLoudnessStatus(
        runtime, "cached_music", AudioManager::LoudnessStatus::Ready));
    const auto& ready = audioManager.GetAllSounds().at("cached_music");
    EXPECT_FLOAT_EQ(ready.integratedLufs, -20.0f);
    EXPECT_FLOAT_EQ(ready.normalizationGainDb, 4.0f);
    EXPECT_GT(ready.normalizationGainLinear, 1.0f);

    CliCommandProcessor processor(runtime);
    const CliResult clearResult =
        processor.Execute("loudness.clear-cache", nlohmann::json::object());
    ASSERT_TRUE(clearResult.IsSuccess()) << clearResult.errorMessage;
    EXPECT_TRUE(clearResult.data.at("removed"));
    const auto& cleared = audioManager.GetAllSounds().at("cached_music");
    EXPECT_EQ(cleared.loudnessStatus, AudioManager::LoudnessStatus::NotQueued);
    EXPECT_FLOAT_EQ(cleared.integratedLufs, 0.0f);
    EXPECT_FLOAT_EQ(cleared.truePeakDb, 0.0f);
    EXPECT_FLOAT_EQ(cleared.normalizationGainDb, 0.0f);
    EXPECT_FLOAT_EQ(cleared.normalizationGainLinear, 1.0f);

    cache[cacheKey] = ValidLoudnessCacheEntry(wave, -24.0f, -12.0f);
    WriteJson(cachePath, cache);
    audioManager.QueueLoudnessAnalysis("cached_music");
    ASSERT_TRUE(WaitForLoudnessStatus(
        runtime, "cached_music", AudioManager::LoudnessStatus::Ready));
    const auto& reanalyzed = audioManager.GetAllSounds().at("cached_music");
    EXPECT_FLOAT_EQ(reanalyzed.integratedLufs, -24.0f);
    EXPECT_FLOAT_EQ(reanalyzed.normalizationGainDb, 8.0f);
}

TEST_F(CliRuntimeTests, ClearLoudnessCacheCancelsQueuedAnalysis)
{
    const std::filesystem::path wave = CreateWave();
    const std::filesystem::path cachePath = CreateTemporaryPath("_loudness.json");
    const std::string cacheKey = NormalizedCacheKey(wave);
    auto& audioManager = AudioManager::GetInstance();
    ASSERT_TRUE(audioManager.LoadSound(
        "queued_music", wave.string(), true, AudioManager::SoundKind::Music));
    audioManager.ConfigureLoudness(cachePath.string(), -16.0f);
    nlohmann::json cache = nlohmann::json::object();
    cache[cacheKey] = ValidLoudnessCacheEntry(wave, -20.0f, -6.0f);
    WriteJson(cachePath, cache);
    audioManager.QueueLoudnessAnalysis("queued_music");

    CliCommandProcessor processor(runtime);
    const CliResult result =
        processor.Execute("loudness.clear-cache", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_TRUE(result.data.at("removed"));
    EXPECT_EQ(
        audioManager.GetAllSounds().at("queued_music").loudnessStatus,
        AudioManager::LoudnessStatus::NotQueued);

    audioManager.QueueLoudnessAnalysis("queued_music");
    EXPECT_EQ(
        audioManager.GetAllSounds().at("queued_music").loudnessStatus,
        AudioManager::LoudnessStatus::Queued);
    bool removed = false;
    std::string clearError;
    ASSERT_TRUE(audioManager.ClearLoudnessCache(removed, clearError)) << clearError;
}

TEST_F(CliRuntimeTests, SoundPlaybackControlsWorkWithNoSoundBackend)
{
    const std::filesystem::path wave = CreateWave();
    CliCommandProcessor processor(runtime);
    CliResult result = processor.Execute(
        "sound.load",
        {{"id", "test_sound"}, {"path", wave.string()}, {"kind", "sfx"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;

    result = processor.Execute(
        "sound.play",
        {{"id", "test_sound"}, {"volume", 0.5}, {"pitch", 1.5}, {"loop", true}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_NEAR(result.data.at("channel").at("pitch").get<double>(), 1.5, 0.001);
    runtime.Tick(0.01f);

    result = processor.Execute(
        "sound.set", {{"id", "test_sound"}, {"pitch", 0.75}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    ASSERT_FALSE(result.data.at("sound").at("channels").empty());
    EXPECT_NEAR(
        result.data.at("sound").at("channels").at(0).at("pitch").get<double>(),
        0.75, 0.001);

    result = processor.Execute("sound.pause", {{"id", "test_sound"}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute("sound.resume", {{"id", "test_sound"}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute(
        "sound.seek", {{"id", "test_sound"}, {"position_ms", 25}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute("sound.stop", {{"id", "test_sound"}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
    result = processor.Execute("sound.unload", {{"id", "test_sound"}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
}

TEST_F(CliRuntimeTests, ReusedChannelHandlesNeverMutateAnotherSound)
{
    const std::filesystem::path wave = CreateWave();
    auto& audio = AudioManager::GetInstance();
    ASSERT_TRUE(audio.LoadSound("stale_owner", wave.string()));
    ASSERT_TRUE(audio.LoadSound("live_owner", wave.string()));

    FMOD::Channel* liveChannel = audio.PlaySound("live_owner", true, 0.7f, 1.25f);
    ASSERT_NE(liveChannel, nullptr);
    auto& sounds = const_cast<std::map<std::string, AudioManager::SoundData>&>(
        audio.GetAllSounds());
    auto injectStaleHandle = [&]
    {
        sounds.at("stale_owner").channels.push_back(liveChannel);
    };

    injectStaleHandle();
    audio.SetVolume("stale_owner", 0.1f);
    float volume = 0.0f;
    ASSERT_EQ(liveChannel->getVolume(&volume), FMOD_OK);
    EXPECT_NEAR(volume, 0.7f, 0.001f);

    injectStaleHandle();
    audio.SetPitch("stale_owner", 0.5f);
    float pitch = 0.0f;
    ASSERT_EQ(liveChannel->getPitch(&pitch), FMOD_OK);
    EXPECT_NEAR(pitch, 1.25f, 0.001f);

    injectStaleHandle();
    audio.StopSound("stale_owner");
    bool playing = false;
    ASSERT_EQ(liveChannel->isPlaying(&playing), FMOD_OK);
    EXPECT_TRUE(playing);

    injectStaleHandle();
    audio.StopSoundWithFadeOut("stale_owner");
    runtime.Tick(2.0f);
    ASSERT_EQ(liveChannel->isPlaying(&playing), FMOD_OK);
    EXPECT_TRUE(playing);

    injectStaleHandle();
    CliCommandProcessor processor(runtime);
    const CliResult pauseResult =
        processor.Execute("sound.pause", {{"id", "stale_owner"}});
    EXPECT_FALSE(pauseResult.IsSuccess());
    EXPECT_EQ(pauseResult.errorCode, "sound_not_playing");
    bool paused = false;
    ASSERT_EQ(liveChannel->getPaused(&paused), FMOD_OK);
    EXPECT_FALSE(paused);

    const CliResult showResult =
        processor.Execute("sound.show", {{"id", "stale_owner"}});
    ASSERT_TRUE(showResult.IsSuccess()) << showResult.errorMessage;
    EXPECT_EQ(showResult.data.at("sound").at("playingChannels"), 0);
    EXPECT_TRUE(showResult.data.at("sound").at("channels").empty());
}

TEST_F(CliRuntimeTests, SafetyGateHardStopsNormalAudioButPreservesEmergencyPreset)
{
    const std::filesystem::path wave = CreateWave();
    auto& audio = AudioManager::GetInstance();
    ASSERT_TRUE(audio.LoadSound(
        "normal_music", wave.string(), true, AudioManager::SoundKind::Music));
    ASSERT_TRUE(audio.LoadEmergencySound("evacuation_preset", wave.string()));

    FMOD::Channel* normal = audio.PlayMusic("normal_music", true, 0.8f);
    FMOD::Channel* emergency = audio.PlaySound("evacuation_preset", true, 1.0f);
    ASSERT_NE(normal, nullptr);
    ASSERT_NE(emergency, nullptr);

    audio.SetNormalPlaybackBlocked(true);
    audio.StopAllNonEmergencyImmediately();

    bool playing = true;
    const FMOD_RESULT normalState = normal->isPlaying(&playing);
    EXPECT_TRUE(normalState != FMOD_OK || !playing);
    ASSERT_EQ(emergency->isPlaying(&playing), FMOD_OK);
    EXPECT_TRUE(playing);
    EXPECT_EQ(audio.PlayMusic("normal_music", true), nullptr);
    EXPECT_NE(audio.PlaySound("evacuation_preset", true), nullptr);

    audio.SetNormalPlaybackBlocked(false);
    EXPECT_NE(audio.PlayMusic("normal_music", true), nullptr);
}

TEST_F(CliRuntimeTests, CinemaSafetyCliLatchesResetsAndRequiresExplicitResume)
{
    const std::filesystem::path music = CreateWave();
    const std::filesystem::path evacuation = CreateWave();
    const std::filesystem::path configPath = CreateTemporaryPath("_cinema.json");
    WriteJson(configPath, {
        {"playlists", nlohmann::json::array({{
            {"name", "cinema"},
            {"options", {{"loopPlaylist", true}}},
            {"tracks", nlohmann::json::array({{
                {"id", "cinema-track"}, {"path", music.string()}
            }})}
        }})},
        {"announcements", nlohmann::json::array({{
            {"id", "evacuation"}, {"path", evacuation.string()}
        }})},
        {"cinema", {
            {"enabled", true},
            {"schedules", nlohmann::json::array({{
                {"id", "always"}, {"playlist", "cinema"}
            }})},
            {"resume", {
                {"enabled", true}, {"automatic", true},
                {"maxAgeMinutes", 60}, {"checkpointIntervalSeconds", 1}
            }},
            {"safety", {
                {"enabled", true},
                {"evacuationAnnouncementId", "evacuation"},
                {"interlock", {
                    {"enabled", true},
                    {"expectedHeartbeatSource", "fire-panel"},
                    {"heartbeatTimeoutMilliseconds", 1000},
                    {"startupGraceMilliseconds", 10000},
                    {"safeStableMilliseconds", 0}
                }}
            }}
        }}
    });

    ASSERT_TRUE(runtime.ReloadConfiguration(configPath.string(), error)) << error;
    CliCommandProcessor processor(runtime);

    CliResult result = processor.Execute("safety.status", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_EQ(result.data.at("safety").at("mode"), "inhibited");

    result = processor.Execute("safety.heartbeat", {
        {"source_id", "intruder"}, {"session_id", "boot-1"},
        {"sequence", 1}, {"state", "safe"}
    });
    EXPECT_FALSE(result.IsSuccess());
    EXPECT_EQ(result.errorCode, "unexpected_heartbeat_source");

    result = processor.Execute("safety.heartbeat", {
        {"source_id", "fire-panel"}, {"session_id", "boot-1"},
        {"sequence", 1}, {"state", "safe"}
    });
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    runtime.Tick(0.01f);
    ASSERT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema"));

    result = processor.Execute("safety.heartbeat", {
        {"source_id", "fire-panel"}, {"session_id", "boot-1"},
        {"sequence", 2}, {"state", "alarm"}
    });
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_EQ(result.data.at("safety").at("mode"), "latched");
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema"));

    result = processor.Execute("sound.play", {{"id", "cinema-track"}});
    EXPECT_FALSE(result.IsSuccess());
    EXPECT_EQ(result.errorCode, "playback_inhibited");

    result = processor.Execute("safety.heartbeat", {
        {"source_id", "fire-panel"}, {"session_id", "boot-1"},
        {"sequence", 3}, {"state", "safe"}
    });
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    const CliResult latched = processor.Execute(
        "safety.status", nlohmann::json::object());
    ASSERT_TRUE(latched.IsSuccess());
    const std::string incidentId =
        latched.data.at("safety").at("incident").at("id").get<std::string>();

    result = processor.Execute("safety.reset", {
        {"incident_id", incidentId},
        {"operator", "operator-1"},
        {"reason", "Fire panel and auditorium verified safe"}
    });
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    runtime.Tick(0.01f);
    EXPECT_FALSE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema"));

    result = processor.Execute("session.resume", nlohmann::json::object());
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_TRUE(PlaylistManager::GetInstance().IsPlaylistPlaying("cinema"));
}

TEST_F(CliRuntimeTests, AnnouncementSchedulesUseStableIds)
{
    const std::filesystem::path wave = CreateWave();
    CliCommandProcessor processor(runtime);
    CliResult result = processor.Execute(
        "announcement.load", {{"id", "doors"}, {"path", wave.string()}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;

    result = processor.Execute(
        "schedule.add", {{"announcement", "doors"}, {"at", "12:30"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    const std::uint64_t scheduleId =
        result.data.at("schedule").at("scheduleId").get<std::uint64_t>();
    EXPECT_GT(scheduleId, 0u);

    result = processor.Execute(
        "schedule.update",
        {{"schedule_id", scheduleId}, {"announcement", "doors"}, {"at", "13:45"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_EQ(result.data.at("schedule").at("hour"), 13);
    EXPECT_EQ(result.data.at("schedule").at("minute"), 45);

    result = processor.Execute(
        "schedule.update", {{"schedule_id", scheduleId}, {"hour", 14}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_EQ(result.data.at("schedule").at("hour"), 14);
    EXPECT_EQ(result.data.at("schedule").at("minute"), 45);

    result = processor.Execute(
        "schedule.update",
        {{"schedule_id", scheduleId}, {"at", "15:00"}, {"minute", 30}});
    EXPECT_FALSE(result.IsSuccess());
    EXPECT_EQ(result.errorCode, "ambiguous_time");

    result = processor.Execute("schedule.remove", {{"schedule_id", scheduleId}});
    EXPECT_TRUE(result.IsSuccess()) << result.errorMessage;
}

TEST_F(CliRuntimeTests, InactivePlaylistStatusDoesNotLeakActiveMetrics)
{
    const std::filesystem::path wave = CreateWave();
    CliCommandProcessor processor(runtime);
    ASSERT_TRUE(processor.Execute(
        "sound.load",
        {{"id", "active_track"}, {"path", wave.string()}, {"kind", "music"}})
                    .IsSuccess());
    ASSERT_TRUE(processor.Execute("playlist.create", {{"name", "active"}}).IsSuccess());
    ASSERT_TRUE(processor.Execute("playlist.create", {{"name", "inactive"}}).IsSuccess());
    ASSERT_TRUE(processor.Execute(
        "playlist.add", {{"name", "active"}, {"id", "active_track"}})
                    .IsSuccess());
    ASSERT_TRUE(processor.Execute("playlist.play", {{"name", "active"}}).IsSuccess());

    const CliResult result = processor.Execute(
        "playlist.status", {{"name", "inactive"}});
    ASSERT_TRUE(result.IsSuccess()) << result.errorMessage;
    EXPECT_TRUE(result.data.at("active").at("currentTrack").is_null());
    EXPECT_EQ(result.data.at("active").at("trackProgress"), 0.0);
}

TEST(CliRuntimeLifecycleTests, ReinitializeWithoutConfigClearsSessionMetadata)
{
    const std::filesystem::path config = UniqueTemporaryPath(".json");
    {
        std::ofstream output(config);
        output << R"({"loudnessTargetLufs":-20,"playlists":[],"announcements":[]})";
    }

    ApplicationRuntime runtime;
    RuntimeOptions options;
    options.configPath = config.string();
    options.noSound = true;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(options, error)) << error;
    EXPECT_FALSE(runtime.GetConfigPath().empty());
    EXPECT_FLOAT_EQ(AudioManager::GetInstance().GetLoudnessTarget(), -20.0f);
    runtime.Shutdown();

    options.loadConfig = false;
    ASSERT_TRUE(runtime.Initialize(options, error)) << error;
    EXPECT_TRUE(runtime.GetConfigPath().empty());
    EXPECT_TRUE(runtime.GetResourceRoot().empty());
    EXPECT_TRUE(runtime.GetLoadReport().failures.empty());
    EXPECT_FLOAT_EQ(AudioManager::GetInstance().GetLoudnessTarget(), -16.0f);
    runtime.Shutdown();

    std::error_code removeError;
    std::filesystem::remove(config, removeError);
}

TEST(CliRuntimePathTests, DefaultConfigIsResolvedBesideExecutable)
{
    const std::filesystem::path root = UniqueTemporaryPath("_install");
    const std::filesystem::path configDirectory = root / "config";
    std::filesystem::create_directories(configDirectory);
    const std::filesystem::path config = configDirectory / "tsm_config.json";
    {
        std::ofstream output(config);
        output << "{}";
    }
    const std::filesystem::path resolved = ResolveDefaultConfigPath(
        (root / "TheaterSoundManager.exe").string());
    EXPECT_EQ(std::filesystem::path(resolved).lexically_normal(), config.lexically_normal());
    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
}

} // namespace TSM::Tests
