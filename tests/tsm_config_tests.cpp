#include "pch.h"
#include "tsm_config.h"

#include <filesystem>

namespace TSM::Tests
{

TEST(ConfigTests, LoadsPlaylistLoudnessAndSchedule)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "tsm_config_test.json";
    {
        std::ofstream output(path);
        output << R"({
            "loudnessTargetLufs": -18.0,
            "playlists": [{
                "name": "test",
                "options": {"segmentDuration": 42.0},
                "tracks": [{"id": "track", "path": "track.mp3"}]
            }],
            "announcements": [{
                "id": "announcement", "path": "announcement.mp3",
                "hour": 9, "minute": 30
            }]
        })";
    }

    AppConfig config;
    std::string error;
    ASSERT_TRUE(LoadAppConfig(path.string(), config, error)) << error;
    EXPECT_FLOAT_EQ(config.loudnessTargetLufs, -18.0f);
    ASSERT_EQ(config.playlists.size(), 1u);
    EXPECT_EQ(config.playlists[0].name, "test");
    EXPECT_FLOAT_EQ(config.playlists[0].options.segmentDuration, 42.0f);
    ASSERT_EQ(config.playlists[0].tracks.size(), 1u);
    ASSERT_EQ(config.announcements.size(), 1u);
    EXPECT_EQ(config.announcements[0].hour, 9);
    EXPECT_EQ(config.announcements[0].minute, 30);

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
}

} // namespace TSM::Tests
