#include "pch.h"

namespace TSM {
    namespace Tests {

        class AudioManagerLogicTests : public ::testing::Test {
        protected:
            void SetUp() override {
            }

            void TearDown() override {
            }

            struct FakeSoundData {
                std::string filePath;
            };

            std::map<std::string, FakeSoundData> GetAllLoadedSounds() {
                std::map<std::string, FakeSoundData> result;
                const auto& audioManager = AudioManager::GetInstance();
                const auto& allSounds = audioManager.GetAllSounds();

                for (const auto& [id, data] : allSounds) {
                    result[id] = { data.filePath };
                }

                return result;
            }
        };

        TEST_F(AudioManagerLogicTests, LoadWeddingPhaseSound) {
            auto& audioManager = AudioManager::GetInstance();

            std::string testFilePath = "test_wedding_phase.mp3";
            bool result = audioManager.LoadWeddingPhaseSound(1, testFilePath);

            auto allSounds = GetAllLoadedSounds();
            auto it = allSounds.find("wedding_entrance_sound");

            if (result) {
                ASSERT_NE(it, allSounds.end());
                if (it != allSounds.end()) {
                    ASSERT_EQ(it->second.filePath, testFilePath);
                }
            }
        }

        TEST_F(AudioManagerLogicTests, LoadAnnouncement) {
            auto& audioManager = AudioManager::GetInstance();

            std::string announcementId = "test_announcement";
            std::string testFilePath = "test_announcement.mp3";
            bool result = audioManager.LoadAnnouncement(announcementId, testFilePath);

            auto allSounds = GetAllLoadedSounds();
            auto it = allSounds.find(announcementId);

            if (result) {
                ASSERT_NE(it, allSounds.end());
                if (it != allSounds.end()) {
                    ASSERT_EQ(it->second.filePath, testFilePath);
                }
            }
        }

    }
}