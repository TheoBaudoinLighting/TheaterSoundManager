#include "pch.h"

#include <cstdint>

namespace TSM {
    namespace Tests {

        namespace {

        void WriteLittleEndian16(std::ofstream& output, std::uint16_t value) {
            const char bytes[] = {
                static_cast<char>(value & 0xffu),
                static_cast<char>((value >> 8u) & 0xffu)
            };
            output.write(bytes, sizeof(bytes));
        }

        void WriteLittleEndian32(std::ofstream& output, std::uint32_t value) {
            const char bytes[] = {
                static_cast<char>(value & 0xffu),
                static_cast<char>((value >> 8u) & 0xffu),
                static_cast<char>((value >> 16u) & 0xffu),
                static_cast<char>((value >> 24u) & 0xffu)
            };
            output.write(bytes, sizeof(bytes));
        }

        void WriteSilentWave(const std::filesystem::path& path) {
            constexpr std::uint32_t sampleRate = 8000;
            constexpr std::uint16_t channels = 1;
            constexpr std::uint16_t bitsPerSample = 16;
            constexpr std::uint32_t sampleCount = 800;
            constexpr std::uint32_t dataSize =
                sampleCount * channels * (bitsPerSample / 8u);

            std::ofstream output(path, std::ios::binary);
            ASSERT_TRUE(output.is_open());
            output.write("RIFF", 4);
            WriteLittleEndian32(output, 36u + dataSize);
            output.write("WAVEfmt ", 8);
            WriteLittleEndian32(output, 16);
            WriteLittleEndian16(output, 1);
            WriteLittleEndian16(output, channels);
            WriteLittleEndian32(output, sampleRate);
            WriteLittleEndian32(
                output, sampleRate * channels * (bitsPerSample / 8u));
            WriteLittleEndian16(output, channels * (bitsPerSample / 8u));
            WriteLittleEndian16(output, bitsPerSample);
            output.write("data", 4);
            WriteLittleEndian32(output, dataSize);
            const std::vector<char> silence(dataSize, 0);
            output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
        }

        } // namespace

        class PlaylistManagerTests : public ::testing::Test {
        protected:
            void SetUp() override {
                m_playlistName = "test_playlist";

                auto& manager = PlaylistManager::GetInstance();

                for (const auto& name : manager.GetPlaylistNames()) {
                    manager.DeletePlaylist(name);
                }
            }

            void TearDown() override {
                auto& manager = PlaylistManager::GetInstance();

                for (const auto& name : manager.GetPlaylistNames()) {
                    manager.DeletePlaylist(name);
                }

                for (const auto& path : m_temporaryPaths) {
                    std::error_code removeError;
                    std::filesystem::remove(path, removeError);
                }
            }

            std::filesystem::path TemporaryPath(const char* suffix) {
                const auto stamp = std::chrono::high_resolution_clock::now()
                    .time_since_epoch().count();
                const auto path = std::filesystem::temp_directory_path() /
                    ("tsm_playlist_" + std::to_string(stamp) + suffix);
                m_temporaryPaths.push_back(path);
                return path;
            }

            std::string m_playlistName;
            std::vector<std::filesystem::path> m_temporaryPaths;
        };

        TEST_F(PlaylistManagerTests, CreatePlaylist) {
            auto& manager = PlaylistManager::GetInstance();

            manager.CreatePlaylist(m_playlistName);

            auto playlists = manager.GetPlaylistNames();
            ASSERT_NE(std::find(playlists.begin(), playlists.end(), m_playlistName), playlists.end());
        }

        TEST_F(PlaylistManagerTests, DeletePlaylist) {
            auto& manager = PlaylistManager::GetInstance();

            manager.CreatePlaylist(m_playlistName);
            manager.DeletePlaylist(m_playlistName);

            auto playlists = manager.GetPlaylistNames();
            ASSERT_EQ(std::find(playlists.begin(), playlists.end(), m_playlistName), playlists.end());
        }

        TEST_F(PlaylistManagerTests, RenamePlaylist) {
            auto& manager = PlaylistManager::GetInstance();
            std::string newName = "renamed_playlist";

            manager.CreatePlaylist(m_playlistName);
            manager.RenamePlaylist(m_playlistName, newName);

            auto playlists = manager.GetPlaylistNames();
            ASSERT_EQ(std::find(playlists.begin(), playlists.end(), m_playlistName), playlists.end());
            ASSERT_NE(std::find(playlists.begin(), playlists.end(), newName), playlists.end());
        }

        TEST_F(PlaylistManagerTests, DuplicatePlaylist) {
            auto& manager = PlaylistManager::GetInstance();
            std::string duplicateName = "duplicate_playlist";

            manager.CreatePlaylist(m_playlistName);
            manager.DuplicatePlaylist(m_playlistName, duplicateName);

            auto playlists = manager.GetPlaylistNames();
            ASSERT_NE(std::find(playlists.begin(), playlists.end(), m_playlistName), playlists.end());
            ASSERT_NE(std::find(playlists.begin(), playlists.end(), duplicateName), playlists.end());
        }

        TEST_F(PlaylistManagerTests, MoveTrackToPosition) {
            auto& manager = PlaylistManager::GetInstance();

            manager.CreatePlaylist(m_playlistName);

            manager.AddToPlaylist(m_playlistName, "track1");
            manager.AddToPlaylist(m_playlistName, "track2");
            manager.AddToPlaylist(m_playlistName, "track3");

            auto* playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            ASSERT_EQ(playlist->tracks.size(), 3);
            ASSERT_EQ(playlist->tracks[0], "track1");
            ASSERT_EQ(playlist->tracks[1], "track2");
            ASSERT_EQ(playlist->tracks[2], "track3");

            manager.MoveTrackToPosition(m_playlistName, 0, 2);

            ASSERT_EQ(playlist->tracks[0], "track2");
            ASSERT_EQ(playlist->tracks[1], "track3");
            ASSERT_EQ(playlist->tracks[2], "track1");
        }

        TEST_F(PlaylistManagerTests, MissingSegmentAssetsStopCleanly) {
            auto& manager = PlaylistManager::GetInstance();

            manager.CreatePlaylist(m_playlistName);
            manager.AddToPlaylist(m_playlistName, "missing_track");

            PlaylistOptions options;
            options.randomOrder = true;
            options.randomSegment = true;
            options.segmentDuration = 150.0f;
            options.loopPlaylist = true;

            manager.Play(m_playlistName, options);

            EXPECT_FALSE(manager.IsPlaylistPlaying(m_playlistName));
            EXPECT_EQ(manager.GetCurrentChannel(), nullptr);
            EXPECT_TRUE(manager.GetCurrentTrackName().empty());
        }

        TEST_F(PlaylistManagerTests, SegmentOptionsRoundTripThroughPlaylistFiles) {
            auto& manager = PlaylistManager::GetInstance();
            manager.CreatePlaylist(m_playlistName);

            auto* playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            playlist->options.randomOrder = false;
            playlist->options.randomSegment = false;
            playlist->options.loopPlaylist = false;
            playlist->options.segmentDuration = 42.0f;
            playlist->options.automaticSegmentDuration = true;
            playlist->options.minSegmentDuration = 45.0f;
            playlist->options.maxSegmentDuration = 240.0f;
            playlist->crossfadeDuration = 4.25f;

            const auto singlePlaylist = TemporaryPath("_single.json");
            ASSERT_TRUE(manager.ExportPlaylist(m_playlistName, singlePlaylist.string()));

            playlist->options.randomOrder = true;
            playlist->options.segmentDuration = 99.0f;
            playlist->options.automaticSegmentDuration = false;
            playlist->options.minSegmentDuration = 10.0f;
            playlist->options.maxSegmentDuration = 20.0f;
            playlist->crossfadeDuration = 0.5f;
            ASSERT_TRUE(manager.ImportPlaylist(singlePlaylist.string(), m_playlistName));

            playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            EXPECT_FALSE(playlist->options.randomOrder);
            EXPECT_FLOAT_EQ(playlist->options.segmentDuration, 42.0f);
            EXPECT_TRUE(playlist->options.automaticSegmentDuration);
            EXPECT_FLOAT_EQ(playlist->options.minSegmentDuration, 45.0f);
            EXPECT_FLOAT_EQ(playlist->options.maxSegmentDuration, 240.0f);
            EXPECT_FLOAT_EQ(playlist->crossfadeDuration, 4.25f);

            const auto allPlaylists = TemporaryPath("_all.json");
            ASSERT_TRUE(manager.SavePlaylistsToFile(allPlaylists.string()));

            playlist->options.randomSegment = true;
            playlist->options.automaticSegmentDuration = false;
            playlist->options.minSegmentDuration = 1.0f;
            playlist->options.maxSegmentDuration = 2.0f;
            playlist->crossfadeDuration = 1.0f;
            ASSERT_TRUE(manager.LoadPlaylistsFromFile(allPlaylists.string()));

            playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            EXPECT_FALSE(playlist->options.randomSegment);
            EXPECT_TRUE(playlist->options.automaticSegmentDuration);
            EXPECT_FLOAT_EQ(playlist->options.minSegmentDuration, 45.0f);
            EXPECT_FLOAT_EQ(playlist->options.maxSegmentDuration, 240.0f);
            EXPECT_FLOAT_EQ(playlist->crossfadeDuration, 4.25f);
        }

        TEST_F(PlaylistManagerTests, LegacyPlaylistImportUsesAutomaticSegmentDefaults) {
            auto& manager = PlaylistManager::GetInstance();
            manager.CreatePlaylist(m_playlistName);

            const auto legacyPlaylist = TemporaryPath("_legacy.json");
            const nlohmann::json document = {
                {"name", m_playlistName},
                {"options", {
                    {"randomSegment", true},
                    {"segmentDuration", 42.0}
                }},
                {"tracks", nlohmann::json::array()}
            };
            {
                std::ofstream output(legacyPlaylist);
                ASSERT_TRUE(output.is_open());
                output << document.dump(2);
            }

            ASSERT_TRUE(manager.ImportPlaylist(
                legacyPlaylist.string(), m_playlistName));

            const auto* playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            EXPECT_TRUE(playlist->options.randomSegment);
            EXPECT_FLOAT_EQ(playlist->options.segmentDuration, 42.0f);
            EXPECT_FALSE(playlist->options.automaticSegmentDuration);
            EXPECT_FLOAT_EQ(playlist->options.minSegmentDuration, 45.0f);
            EXPECT_FLOAT_EQ(playlist->options.maxSegmentDuration, 240.0f);
        }

        TEST_F(PlaylistManagerTests, RejectsReversedAutomaticSegmentRangeOnImport) {
            auto& manager = PlaylistManager::GetInstance();
            manager.CreatePlaylist(m_playlistName);

            auto* playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            playlist->options.automaticSegmentDuration = false;
            playlist->options.minSegmentDuration = 60.0f;
            playlist->options.maxSegmentDuration = 120.0f;

            const auto invalidPlaylist = TemporaryPath("_invalid_range.json");
            const nlohmann::json document = {
                {"name", m_playlistName},
                {"options", {
                    {"automaticSegmentDuration", true},
                    {"minSegmentDuration", 240.0},
                    {"maxSegmentDuration", 45.0}
                }},
                {"tracks", nlohmann::json::array()}
            };
            {
                std::ofstream output(invalidPlaylist);
                ASSERT_TRUE(output.is_open());
                output << document.dump(2);
            }

            EXPECT_FALSE(manager.ImportPlaylist(
                invalidPlaylist.string(), m_playlistName));

            playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            EXPECT_FALSE(playlist->options.automaticSegmentDuration);
            EXPECT_FLOAT_EQ(playlist->options.minSegmentDuration, 60.0f);
            EXPECT_FLOAT_EQ(playlist->options.maxSegmentDuration, 120.0f);
        }

        TEST_F(PlaylistManagerTests, FailedImportPreservesPlaylistAndRollsBackSounds) {
            ApplicationRuntime runtime;
            RuntimeOptions runtimeOptions;
            runtimeOptions.loadConfig = false;
            runtimeOptions.noSound = true;
            std::string runtimeError;
            ASSERT_TRUE(runtime.Initialize(runtimeOptions, runtimeError)) << runtimeError;

            auto& manager = PlaylistManager::GetInstance();
            manager.CreatePlaylist(m_playlistName);
            manager.AddToPlaylist(m_playlistName, "original_track");
            auto* playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            playlist->options.randomOrder = false;
            playlist->crossfadeDuration = 7.0f;
            playlist->isPlaying = true;

            const auto wave = TemporaryPath(".wav");
            WriteSilentWave(wave);
            const auto documentPath = TemporaryPath("_rollback.json");
            const auto missingWave = TemporaryPath("_missing.wav");
            const nlohmann::json document = {
                {"name", m_playlistName},
                {"options", {
                    {"randomOrder", true},
                    {"crossfadeDuration", 2.0}
                }},
                {"tracks", nlohmann::json::array({
                    {{"id", "loaded_then_rolled_back"}, {"path", wave.string()}},
                    {{"id", "unavailable_track"}, {"path", missingWave.string()}}
                })}
            };
            {
                std::ofstream output(documentPath);
                ASSERT_TRUE(output.is_open());
                output << document.dump(2);
            }

            EXPECT_FALSE(manager.ImportPlaylist(documentPath.string(), m_playlistName));

            playlist = manager.GetPlaylistByName(m_playlistName);
            ASSERT_NE(playlist, nullptr);
            ASSERT_EQ(playlist->tracks.size(), 1u);
            EXPECT_EQ(playlist->tracks.front(), "original_track");
            EXPECT_FALSE(playlist->options.randomOrder);
            EXPECT_FLOAT_EQ(playlist->crossfadeDuration, 7.0f);
            EXPECT_TRUE(playlist->isPlaying);
            EXPECT_FALSE(AudioManager::GetInstance().GetAllSounds().contains(
                "loaded_then_rolled_back"));
            EXPECT_FALSE(AudioManager::GetInstance().GetAllSounds().contains(
                "unavailable_track"));
        }

        TEST_F(PlaylistManagerTests, LibraryPlaybackUsesEveryImportedMusicTrackOnce) {
            ApplicationRuntime runtime;
            RuntimeOptions runtimeOptions;
            runtimeOptions.loadConfig = false;
            runtimeOptions.noSound = true;
            std::string runtimeError;
            ASSERT_TRUE(runtime.Initialize(runtimeOptions, runtimeError)) << runtimeError;

            const auto wave = TemporaryPath("_library.wav");
            WriteSilentWave(wave);

            auto& audio = AudioManager::GetInstance();
            ASSERT_TRUE(audio.LoadSound(
                "library_member_music", wave.string(), true,
                AudioManager::SoundKind::Music));
            ASSERT_TRUE(audio.LoadSound(
                "library_orphan_music", wave.string(), true,
                AudioManager::SoundKind::Music));
            ASSERT_TRUE(audio.LoadSound(
                "library_announcement", wave.string(), true,
                AudioManager::SoundKind::Announcement));
            ASSERT_TRUE(audio.LoadSound(
                "library_effect", wave.string(), false,
                AudioManager::SoundKind::SoundEffect));
            auto& manager = PlaylistManager::GetInstance();
            manager.CreatePlaylist(m_playlistName);
            manager.CreatePlaylist("second_library_reference");
            manager.AddToPlaylist(m_playlistName, "library_member_music");
            manager.AddToPlaylist(
                "second_library_reference", "library_member_music");

            EXPECT_EQ(
                manager.GetLibraryMusicIds(),
                (std::vector<std::string>{
                    "library_member_music", "library_orphan_music"}));

            PlaylistOptions options;
            options.randomOrder = false;
            options.randomSegment = false;
            options.loopPlaylist = true;
            options.segmentDuration = 42.0f;
            manager.ConfigureLibraryPlayback(options, 2.5f);
            EXPECT_FALSE(manager.GetLibraryOptions().randomOrder);
            EXPECT_FALSE(manager.GetLibraryOptions().randomSegment);
            EXPECT_TRUE(manager.GetLibraryOptions().loopPlaylist);
            EXPECT_FLOAT_EQ(manager.GetLibraryOptions().segmentDuration, 42.0f);
            EXPECT_FLOAT_EQ(manager.GetLibraryCrossfadeDuration(), 2.5f);
            ASSERT_TRUE(manager.PlayLibrary(options, 2.5f));
            EXPECT_TRUE(manager.IsLibraryPlaying());
            EXPECT_FALSE(manager.IsPlaylistPlaying(m_playlistName));
            ASSERT_NE(manager.GetActivePlaylist(), nullptr);
            EXPECT_EQ(
                manager.GetActivePlaylist()->tracks,
                (std::vector<std::string>{
                    "library_member_music", "library_orphan_music"}));
            EXPECT_FLOAT_EQ(manager.GetLibraryCrossfadeDuration(), 2.5f);

            manager.Play(m_playlistName, options);
            EXPECT_FALSE(manager.IsLibraryPlaying());
            EXPECT_TRUE(manager.IsPlaylistPlaying(m_playlistName));

            ASSERT_TRUE(manager.PlayLibrary(options, 1.0f));
            EXPECT_TRUE(manager.IsLibraryPlaying());
            EXPECT_FALSE(manager.IsPlaylistPlaying(m_playlistName));

            manager.AbortImmediately();
            EXPECT_FALSE(manager.IsLibraryPlaying());
            EXPECT_EQ(manager.GetActivePlaylist(), nullptr);
            EXPECT_EQ(manager.GetCurrentChannel(), nullptr);
        }

    }
}
