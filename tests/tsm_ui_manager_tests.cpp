#include "pch.h"

namespace TSM {
    namespace Tests {

        class UIManagerTests : public ::testing::Test {
        protected:
            void SetUp() override {
                ClearPlaylists();
                UIManager::GetInstance().ResetSessionState();
            }

            void TearDown() override {
                ClearPlaylists();
                UIManager::GetInstance().ResetSessionState();
            }

            static void ClearPlaylists() {
                auto& playlists = PlaylistManager::GetInstance();
                for (const std::string& name : playlists.GetPlaylistNames()) {
                    playlists.DeletePlaylist(name);
                }
            }
        };

        TEST_F(UIManagerTests, VolumeControls) {
            auto& manager = UIManager::GetInstance();

            manager.SetMasterVolume(0.75f);
            manager.SetMusicVolume(0.5f);
            manager.SetAnnouncementVolume(1.0f);
            manager.SetSFXVolume(0.25f);

            ASSERT_FLOAT_EQ(manager.GetMasterVolume(), 0.75f);
            ASSERT_FLOAT_EQ(manager.GetMusicVolume(), 0.5f);
            ASSERT_FLOAT_EQ(manager.GetAnnouncementVolume(), 1.0f);
            ASSERT_FLOAT_EQ(manager.GetSFXVolume(), 0.25f);
        }

        TEST_F(UIManagerTests, DuckingFactor) {
            auto& manager = UIManager::GetInstance();

            float originalFactor = manager.GetDuckFactor();

            manager.SetDuckFactor(0.3f);
            ASSERT_FLOAT_EQ(manager.GetDuckFactor(), 0.3f);

            manager.SetDuckFactor(1.0f);
            ASSERT_FLOAT_EQ(manager.GetDuckFactor(), 1.0f);

            manager.SetDuckFactor(originalFactor);
        }

        TEST_F(UIManagerTests, VolumeAndDuckingSettersClampUnsafeValues) {
            auto& manager = UIManager::GetInstance();

            manager.SetMasterVolume(-1.0f);
            manager.SetMusicVolume(2.0f);
            manager.SetAnnouncementVolume(4.0f);
            manager.SetSFXVolume(-0.5f);
            manager.SetDuckFactor(2.0f);

            ASSERT_FLOAT_EQ(manager.GetMasterVolume(), 0.0f);
            ASSERT_FLOAT_EQ(manager.GetMusicVolume(), 1.0f);
            ASSERT_FLOAT_EQ(manager.GetAnnouncementVolume(), 3.0f);
            ASSERT_FLOAT_EQ(manager.GetSFXVolume(), 0.0f);
            ASSERT_FLOAT_EQ(manager.GetDuckFactor(), 1.0f);
        }

        TEST_F(UIManagerTests, PlaylistControlsAlwaysResolveToAnExistingPlaylist) {
            auto& playlists = PlaylistManager::GetInstance();
            auto& manager = UIManager::GetInstance();

            playlists.CreatePlaylist("playlist_PostShow");
            playlists.CreatePlaylist("playlist_PreShow");
            manager.RefreshPlaylistSelection();
            EXPECT_EQ(manager.GetSelectedPlaylistName(), "playlist_PreShow");

            playlists.DeletePlaylist("playlist_PreShow");
            manager.RefreshPlaylistSelection();
            EXPECT_EQ(manager.GetSelectedPlaylistName(), "playlist_PostShow");

            playlists.DeletePlaylist("playlist_PostShow");
            manager.RefreshPlaylistSelection();
            EXPECT_TRUE(manager.GetSelectedPlaylistName().empty());
        }

    }
}
