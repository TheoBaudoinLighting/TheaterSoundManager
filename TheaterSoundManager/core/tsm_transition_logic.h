#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace TSM::TransitionLogic
{

enum class Reason
{
    None,
    ManualSkip,
    TrackEnd,
    SegmentEnd,
    AnticipatedTrackEnd,
    AnticipatedSegmentEnd,
    PlaybackFailure
};

struct EqualPowerGains
{
    float outgoing = 1.0f;
    float incoming = 0.0f;
};

struct Decision
{
    bool shouldTransition = false;
    float crossfadeDuration = 0.0f;
    float secondsUntilBoundary = (std::numeric_limits<float>::max)();
    Reason reason = Reason::None;
};

inline EqualPowerGains CalculateEqualPowerGains(float progress)
{
    constexpr float HalfPi = 1.57079632679f;
    const float t = std::clamp(progress, 0.0f, 1.0f);
    return {std::cos(t * HalfPi), std::sin(t * HalfPi)};
}

inline Decision Evaluate(float trackRemaining, float segmentRemaining,
                         bool segmentMode, float configuredCrossfade,
                         bool hasNextTrack, bool isPlaying)
{
    Decision decision;
    if (!isPlaying)
    {
        decision.shouldTransition = true;
        decision.reason = Reason::TrackEnd;
        decision.secondsUntilBoundary = 0.0f;
        return decision;
    }

    const float safeTrackRemaining = std::max(trackRemaining, 0.0f);
    const float safeSegmentRemaining = std::max(segmentRemaining, 0.0f);
    decision.secondsUntilBoundary = segmentMode
        ? std::min(safeTrackRemaining, safeSegmentRemaining)
        : safeTrackRemaining;

    const bool segmentBoundaryFirst = segmentMode &&
        safeSegmentRemaining <= safeTrackRemaining;
    if (decision.secondsUntilBoundary <= 0.0f)
    {
        decision.shouldTransition = true;
        decision.reason = segmentBoundaryFirst ? Reason::SegmentEnd : Reason::TrackEnd;
        return decision;
    }

    const float crossfade = std::max(configuredCrossfade, 0.0f);
    if (hasNextTrack && crossfade > 0.0f && decision.secondsUntilBoundary <= crossfade)
    {
        decision.shouldTransition = true;
        decision.crossfadeDuration = std::min(decision.secondsUntilBoundary, crossfade);
        decision.reason = segmentBoundaryFirst
            ? Reason::AnticipatedSegmentEnd
            : Reason::AnticipatedTrackEnd;
    }
    return decision;
}

inline const char* ToString(Reason reason)
{
    switch (reason)
    {
        case Reason::None: return "none";
        case Reason::ManualSkip: return "manual skip";
        case Reason::TrackEnd: return "track ended";
        case Reason::SegmentEnd: return "segment ended";
        case Reason::AnticipatedTrackEnd: return "anticipated track end";
        case Reason::AnticipatedSegmentEnd: return "anticipated segment end";
        case Reason::PlaybackFailure: return "playback failure";
    }
    return "unknown";
}

} // namespace TSM::TransitionLogic
