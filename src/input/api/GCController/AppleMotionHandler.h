#pragma once
#include "input/motion/MotionHandler.h"

// Keep the Apple sensor frame and a user-selected neutral holding pitch in one
// place. Rotating only acceleration makes sensor fusion fight gyro integration.
class AppleMotionHandler
{
public:
    explicit AppleMotionHandler(GyroBiasMode mode = GyroBiasMode::Estimate) : m_motion(mode) {}

    void reset()
    {
        m_motion.reset();
        m_hasNeutralPitch = false;
        m_cosPitch = 1.0f;
        m_sinPitch = 0.0f;
    }

    void processAppleSample(float dt, float gx, float gy, float gz, float ax, float ay, float az)
    {
        // Apple face-local/screen-local -> Wii U GamePad. Swift has already
        // rotated iPad samples into the current screen orientation.
        const float mappedGy = -gz, mappedGz = gy;
        const float mappedAy = -az, mappedAz = ay;
        if (!m_hasNeutralPitch) {
            const float length2 = ax * ax + mappedAy * mappedAy + mappedAz * mappedAz;
            const float yzLength2 = mappedAy * mappedAy + mappedAz * mappedAz;
            const float rate2 = gx * gx + gy * gy + gz * gz;
            // Wait through freefall, acceleration and active turns. Capture a
            // quiet gravity sample, once on first use or explicit recenter.
            if (length2 >= 0.85f * 0.85f && length2 <= 1.15f * 1.15f &&
                yzLength2 > 0.01f && rate2 < 0.15f * 0.15f) {
                const float inverseLength = 1.0f / std::sqrt(yzLength2);
                m_cosPitch = mappedAy * inverseLength;
                m_sinPitch = mappedAz * inverseLength;
                m_hasNeutralPitch = true;
            }
        }
        m_motion.processMotionSample(dt, gx,
            m_cosPitch * mappedGy + m_sinPitch * mappedGz,
            -m_sinPitch * mappedGy + m_cosPitch * mappedGz,
            ax, m_cosPitch * mappedAy + m_sinPitch * mappedAz,
            -m_sinPitch * mappedAy + m_cosPitch * mappedAz);
    }

    MotionSample getMotionSample() { return m_motion.getMotionSample(); }

private:
    WiiUMotionHandler m_motion;
    bool m_hasNeutralPitch = false;
    float m_cosPitch = 1.0f, m_sinPitch = 0.0f;
};
