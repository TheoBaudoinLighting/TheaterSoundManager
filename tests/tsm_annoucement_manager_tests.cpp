#include "pch.h"

namespace TSM {
    namespace Tests {

        class AnnouncementManagerTests : public ::testing::Test {
        protected:
            void SetUp() override {
                auto& manager = AnnouncementManager::GetInstance();

                while (!manager.GetScheduledAnnouncements().empty()) {
                    manager.RemoveScheduledAnnouncement(0);
                }
            }

            void TearDown() override {
                auto& manager = AnnouncementManager::GetInstance();

                while (!manager.GetScheduledAnnouncements().empty()) {
                    manager.RemoveScheduledAnnouncement(0);
                }
            }
        };

        TEST_F(AnnouncementManagerTests, ScheduleAnnouncement) {
            auto& manager = AnnouncementManager::GetInstance();

            size_t initialCount = manager.GetScheduledAnnouncements().size();
            manager.ScheduleAnnouncement(12, 30, "test_announcement");

            ASSERT_EQ(manager.GetScheduledAnnouncements().size(), initialCount + 1);

            const auto& announcements = manager.GetScheduledAnnouncements();
            ASSERT_FALSE(announcements.empty());

            const auto& lastAnnouncement = announcements.back();
            ASSERT_EQ(lastAnnouncement.hour, 12);
            ASSERT_EQ(lastAnnouncement.minute, 30);
            ASSERT_EQ(lastAnnouncement.announcementId, "test_announcement");
            ASSERT_FALSE(lastAnnouncement.triggered);
        }

        TEST_F(AnnouncementManagerTests, RemoveScheduledAnnouncement) {
            auto& manager = AnnouncementManager::GetInstance();

            manager.ScheduleAnnouncement(12, 30, "test_announcement");
            size_t count = manager.GetScheduledAnnouncements().size();

            manager.RemoveScheduledAnnouncement(count - 1);

            ASSERT_EQ(manager.GetScheduledAnnouncements().size(), count - 1);
        }

        TEST_F(AnnouncementManagerTests, UpdateScheduledAnnouncement) {
            auto& manager = AnnouncementManager::GetInstance();

            manager.ScheduleAnnouncement(12, 30, "test_announcement");
            size_t index = manager.GetScheduledAnnouncements().size() - 1;

            manager.UpdateScheduledAnnouncement(index, 14, 45, "updated_announcement");

            const auto& announcements = manager.GetScheduledAnnouncements();
            ASSERT_FALSE(announcements.empty());

            const auto& updatedAnnouncement = announcements[index];
            ASSERT_EQ(updatedAnnouncement.hour, 14);
            ASSERT_EQ(updatedAnnouncement.minute, 45);
            ASSERT_EQ(updatedAnnouncement.announcementId, "updated_announcement");
            ASSERT_FALSE(updatedAnnouncement.triggered);
        }

        TEST_F(AnnouncementManagerTests, ResetTriggeredAnnouncements) {
            auto& manager = AnnouncementManager::GetInstance();

            manager.ScheduleAnnouncement(12, 30, "test_announcement1");
            manager.ScheduleAnnouncement(14, 45, "test_announcement2");

            auto& announcements = const_cast<std::vector<AnnouncementManager::ScheduledAnnouncement>&>(
                manager.GetScheduledAnnouncements());

            if (!announcements.empty()) {
                announcements[0].triggered = true;
            }

            manager.ResetTriggeredAnnouncements();

            for (const auto& announcement : manager.GetScheduledAnnouncements()) {
                ASSERT_FALSE(announcement.triggered);
            }
        }

        TEST_F(AnnouncementManagerTests, GetAnnouncementStateString) {
            auto& manager = AnnouncementManager::GetInstance();

            ASSERT_EQ(manager.GetAnnouncementStateString(), "Idle");
        }

    }
}