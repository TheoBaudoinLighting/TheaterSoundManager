#include "pch.h"

namespace TSM {
    namespace Tests {

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
            }

            std::string m_playlistName;
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

    }
}