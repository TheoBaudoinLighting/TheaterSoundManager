#include "pch.h"
#include "tsm_transition_logic.h"

namespace TSM::Tests
{

TEST(TransitionLogicTests, EqualPowerMaintainsConstantCombinedPower)
{
    const auto gains = TransitionLogic::CalculateEqualPowerGains(0.5f);
    EXPECT_NEAR(gains.outgoing, 0.7071067f, 0.0001f);
    EXPECT_NEAR(gains.incoming, 0.7071067f, 0.0001f);
    EXPECT_NEAR(gains.outgoing * gains.outgoing + gains.incoming * gains.incoming,
                1.0f, 0.0001f);
}

TEST(TransitionLogicTests, EqualPowerCurveEasesInAndOutWithoutChangingPower)
{
    const auto start = TransitionLogic::CalculateEqualPowerGains(0.0f);
    const auto quarter = TransitionLogic::CalculateEqualPowerGains(0.25f);
    const auto threeQuarter = TransitionLogic::CalculateEqualPowerGains(0.75f);
    const auto end = TransitionLogic::CalculateEqualPowerGains(1.0f);

    EXPECT_FLOAT_EQ(start.outgoing, 1.0f);
    EXPECT_FLOAT_EQ(start.incoming, 0.0f);
    EXPECT_NEAR(end.outgoing, 0.0f, 0.000001f);
    EXPECT_FLOAT_EQ(end.incoming, 1.0f);

    // Smoothstep makes the incoming track gentler than a raw linear-time
    // equal-power fade during the first quarter, with a symmetric fade-out.
    EXPECT_LT(quarter.incoming, 0.3f);
    EXPECT_NEAR(quarter.incoming, threeQuarter.outgoing, 0.0001f);
    EXPECT_NEAR(
        quarter.outgoing * quarter.outgoing +
            quarter.incoming * quarter.incoming,
        1.0f, 0.0001f);
}

TEST(TransitionLogicTests, AnticipatesShortTrackEnd)
{
    const auto decision = TransitionLogic::Evaluate(5.0f, 150.0f, true, 10.0f, true, true);
    EXPECT_TRUE(decision.shouldTransition);
    EXPECT_FLOAT_EQ(decision.crossfadeDuration, 5.0f);
    EXPECT_EQ(decision.reason, TransitionLogic::Reason::AnticipatedTrackEnd);
}

TEST(TransitionLogicTests, AnticipatesSegmentBoundary)
{
    const auto decision = TransitionLogic::Evaluate(100.0f, 7.0f, true, 10.0f, true, true);
    EXPECT_TRUE(decision.shouldTransition);
    EXPECT_FLOAT_EQ(decision.crossfadeDuration, 7.0f);
    EXPECT_EQ(decision.reason, TransitionLogic::Reason::AnticipatedSegmentEnd);
}

TEST(TransitionLogicTests, DoesNotCutLastTrackEarlyWithoutSuccessor)
{
    const auto decision = TransitionLogic::Evaluate(5.0f, 150.0f, true, 10.0f, false, true);
    EXPECT_FALSE(decision.shouldTransition);
    EXPECT_FLOAT_EQ(decision.secondsUntilBoundary, 5.0f);
}

TEST(TransitionLogicTests, EndsImmediatelyAtZeroDurationBoundary)
{
    const auto decision = TransitionLogic::Evaluate(0.0f, 150.0f, true, 0.0f, true, true);
    EXPECT_TRUE(decision.shouldTransition);
    EXPECT_FLOAT_EQ(decision.crossfadeDuration, 0.0f);
    EXPECT_EQ(decision.reason, TransitionLogic::Reason::TrackEnd);
}

TEST(TransitionLogicTests, RecoversWhenPlaybackStopsUnexpectedly)
{
    const auto decision = TransitionLogic::Evaluate(50.0f, 50.0f, true, 10.0f, true, false);
    EXPECT_TRUE(decision.shouldTransition);
    EXPECT_FLOAT_EQ(decision.crossfadeDuration, 0.0f);
    EXPECT_EQ(decision.reason, TransitionLogic::Reason::TrackEnd);
}

} // namespace TSM::Tests
