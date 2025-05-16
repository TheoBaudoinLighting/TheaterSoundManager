#include "pch.h"

namespace TSM {
    namespace Tests {

        class UIManagerTests : public ::testing::Test {
        protected:
            void SetUp() override {
            }

            void TearDown() override {
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

    }
}