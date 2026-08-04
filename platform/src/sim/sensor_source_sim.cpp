#include "platform_sim/sensor_source_sim.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace mark4
{
    namespace
    {
        constexpr float TWO_PI = 6.28318530f;
        constexpr float US_TO_S = 1e-6f;
        constexpr float GYRO_AMPLITUDE_RAD_S = 0.5f;
        constexpr float GYRO_FREQUENCY_HZ = 2.0f;
        constexpr float GRAVITY_MPS2 = 9.80665f;
        constexpr float SEA_LEVEL_PA = 101325.0f;
        constexpr float IDLE_THROTTLE = 0.25f;
    } // namespace

    bool SensorSourceSim::waitFrame(mark4::SensorFrame &frameOut)
    {
        if (m_produced >= m_maxFrames)
        {
            return false;
        }

        // Fixed cadence, standing in for the gyro data-ready interrupt.
        std::this_thread::sleep_for(std::chrono::microseconds(m_periodUs));

        const float tS = static_cast<float>(m_produced) * static_cast<float>(m_periodUs) * US_TO_S;
        const float phase = TWO_PI * GYRO_FREQUENCY_HZ * tS;

        frameOut.timestampUs = m_clock.nowUs();
        frameOut.gyroRadS = {
            GYRO_AMPLITUDE_RAD_S * std::sin(phase), GYRO_AMPLITUDE_RAD_S * std::cos(phase), 0.0f};
        frameOut.accelMps2 = {0.0f, 0.0f, GRAVITY_MPS2}; // specific force at rest
        frameOut.baroPa = SEA_LEVEL_PA;
        frameOut.rc.killSwitch = false;
        frameOut.rc.throttle = IDLE_THROTTLE;

        ++m_produced;
        return true;
    }
} // namespace mark4
