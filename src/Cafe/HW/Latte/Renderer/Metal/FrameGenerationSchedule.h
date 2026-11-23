#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

// A UI setting may change during compositing. Apply it at the next TV-frame
// boundary so a single image never mixes native drawables and FG textures.
class FrameGenerationMode
{
public:
    int ForFrame(int configured)
    {
        if (m_latched < 0)
        {
            if (configured == 0) m_failed = false;
            m_latched = m_failed ? 0 : configured;
        }
        return m_latched;
    }
    void FinishFrame() { m_latched = -1; }
    void Fail() { m_failed = true; m_latched = 0; }
private:
    int m_latched = -1;
    bool m_failed = false;
};

// Called from drawable callbacks: fixed storage avoids allocations on Metal's
// callback threads, including under memory pressure.
class FrameGenerationPresentHistory
{
public:
    bool Add(double time)
    {
        if (!std::isfinite(time) || time <= 0 || (m_count && time <= Latest())) return false;
        m_times[m_next] = time;
        m_next = (m_next + 1) % m_times.size();
        m_count = std::min(m_count + 1, m_times.size());
        while (m_count > 2 && time - Oldest() > 1.0) --m_count;
        return true;
    }
    void Reset() { m_count = m_next = 0; }
    double FPS(double now) const
    {
        return m_count >= 2 && now - Latest() <= .5 ? (m_count - 1) / (Latest() - Oldest()) : 0;
    }
    std::array<double, 2> IntervalRangeMs() const
    {
        if (m_count < 2) return {};
        std::array<double, 2> range{10000, 0};
        for (size_t i = 1; i < m_count; ++i)
        {
            size_t next = (m_next + m_times.size() - m_count + i) % m_times.size();
            size_t previous = (next + m_times.size() - 1) % m_times.size();
            const double interval = (m_times[next] - m_times[previous]) * 1000;
            range[0] = std::min(range[0], interval);
            range[1] = std::max(range[1], interval);
        }
        return range;
    }
private:
    double Latest() const { return m_times[(m_next + m_times.size() - 1) % m_times.size()]; }
    double Oldest() const { return m_times[(m_next + m_times.size() - m_count) % m_times.size()]; }
    std::array<double, 128> m_times{};
    size_t m_count = 0, m_next = 0;
};

// Presentation policy only. This clock must never drive guest VSync or CPU time.
class FrameGenerationSchedule
{
public:
    void Reset()
    {
        m_sourceTime = 0;
        m_outputTime = 0;
        m_nextOutputTime = 0;
        m_outputTarget = 0;
        m_displayPeriod = 0;
        m_interval = 1.0 / 30.0;
        m_samples = 0;
        m_readySource = m_playbackTime = 0;
        m_readyNext = 0;
        m_readyDelays.fill(0);
        m_phaseOffset = 0;
    }

    bool Source(double seconds)
    {
        if (!std::isfinite(seconds) || seconds <= 0)
            return false;
        const double delta = seconds - m_sourceTime;
        const bool discontinuity = m_sourceTime && (delta <= 0 || delta > 0.25);
        if (discontinuity)
            Reset();
        if (m_sourceTime && delta >= 1.0 / 240.0 && delta <= 0.15)
        {
            m_interval = m_samples ? m_interval * 0.9 + delta * 0.1 : delta;
            ++m_samples;
        }
        m_sourceTime = seconds;
        return discontinuity;
    }

    bool Tick(double targetTime, int targetFps)
    {
        if (!std::isfinite(targetTime) || targetTime <= 0 ||
            (targetFps != 30 && targetFps != 60 && targetFps != 120))
            return false;
        const double period = 1.0 / targetFps;
        if (m_outputTime && targetTime <= m_outputTime)
            return false;
        if (m_outputTarget != targetFps)
        {
            m_outputTarget = targetFps;
            m_nextOutputTime = 0;
        }
        if (m_nextOutputTime && targetTime < m_nextOutputTime - period * 0.1)
            return false;
        // Advance the requested cadence, not the last accepted display tick.
        // Otherwise a 144-Hz display reduces a 120-FPS request to 72 FPS.
        m_nextOutputTime = !m_nextOutputTime || targetTime >= m_nextOutputTime + period
            ? targetTime + period : m_nextOutputTime + period;
        m_outputTime = targetTime;
        return true;
    }

    void SetDisplayPeriod(double period)
    {
        if (std::isfinite(period) && period >= 1.0 / 240.0 && period <= 0.1)
            m_displayPeriod = period;
    }

    bool NeedsInterpolation(int targetFps) const
    {
        return m_samples >= 2 && targetFps > 0 && m_interval > OutputPeriod(targetFps) * 1.15;
    }

    double Interval() const { return m_interval; }
    // When the source is an integer fraction of the output rate (60 -> 120,
    // 30 -> 120), outputs can sit on exact phases: every n-th output is an
    // original image (copied, not interpolated), the others use 1/n steps.
    // Returns n, or 0 when the rates do not divide (then positions stay free).
    unsigned PhaseSteps(int targetFps) const
    {
        if (!NeedsInterpolation(targetFps)) return 0;
        const double ratio = m_interval / OutputPeriod(targetFps), steps = std::round(ratio);
        // Only a steady integer ratio locks (60 -> 120 within ~1 FPS). At 55
        // FPS a 2-step lock would repeat about every fifth source frame; free
        // positions keep the motion timing exact instead.
        return steps >= 2 && std::abs(ratio - steps) <= 0.03 * steps ? unsigned(steps) : 0;
    }
    // Phase loop for LockPhase: shift playback slowly toward the exact phase
    // grid. Rounding a position that sits midway between two phases flips
    // between them and shows the same original twice; on the grid it cannot.
    void NudgePhase(double seconds)
    {
        if (std::isfinite(seconds))
            m_phaseOffset = std::clamp(m_phaseOffset + 0.2 * seconds, -0.5 * m_interval, 0.5 * m_interval);
    }
    double PhaseOffset() const { return m_phaseOffset; }
    static double LockPhase(double position, unsigned steps)
    {
        return steps ? std::round(position * steps) / steps : position;
    }
    void Completed(double sourceTime, double completionTime)
    {
        // Submission is not readiness: the source GPU can still take most of
        // a frame to finish. Ignore repeated observations of the same image.
        if (!std::isfinite(sourceTime) || !std::isfinite(completionTime) ||
            sourceTime <= m_readySource || completionTime <= 0)
            return;
        m_readySource = sourceTime;
        m_readyDelays[m_readyNext] = std::clamp(completionTime - sourceTime, 0.0, .15);
        m_readyNext = (m_readyNext + 1) % m_readyDelays.size();
    }
    double ReadyDelay() const { return *std::max_element(m_readyDelays.begin(), m_readyDelays.end()); }
    double PlaybackTime(double displayTime, int targetFps)
    {
        // Both source images must exist before an intermediate image can be
        // constructed. Native-rate output takes the direct/latest-image path.
        if (!NeedsInterpolation(targetFps)) return m_playbackTime = displayTime;
        // Four source slots retain at most three intervals of history. A long
        // one-off GPU stall must not push playback permanently outside it.
        const double desired = displayTime - (m_interval + std::min(ReadyDelay(), m_interval) + OutputPeriod(targetFps) + 0.002) + m_phaseOffset;
        // An increase in source latency may hold a frame, but never rewind
        // motion. Recover the buffer before advancing the timeline again.
        m_playbackTime = std::max(m_playbackTime, desired);
        return m_playbackTime;
    }

    struct Pair
    {
        size_t previous = 0, current = 0;
        double position = 1;
        bool starved = false;
    };
    static Pair Select(std::span<const double> readyTimes, double playback, bool interpolate)
    {
        if (readyTimes.empty()) return {};
        const size_t latest = readyTimes.size() - 1;
        if (!interpolate) return {latest, latest, 1, false};
        if (playback <= readyTimes.front()) return {0, 0, 1, true};
        for (size_t i = 1; i < readyTimes.size(); ++i)
            if (playback <= readyTimes[i])
                return {i-1, i, Position(playback, readyTimes[i-1], readyTimes[i]), false};
        return {latest, latest, 1, true};
    }

    static double Position(double playbackTime, double previousTime, double currentTime)
    {
        const double span = currentTime - previousTime;
        return span > 0 ? std::clamp((playbackTime - previousTime) / span, 0.0, 1.0) : 1.0;
    }

private:
    double OutputPeriod(int targetFps) const { return std::max(1.0 / targetFps, m_displayPeriod); }
    double m_sourceTime = 0;
    double m_outputTime = 0;
    double m_nextOutputTime = 0, m_displayPeriod = 0;
    int m_outputTarget = 0;
    double m_interval = 1.0 / 30.0;
    unsigned m_samples = 0;
    double m_readySource = 0, m_playbackTime = 0;
    std::array<double, 30> m_readyDelays{};
    double m_phaseOffset = 0;
    size_t m_readyNext = 0;
};
