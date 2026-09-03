#pragma once

/// @file
/// @brief The sensor frame as telemetry measures. Every composition steps a
///        SensorFrame it keeps as a local of its loop, so the measures need
///        a stable address of their own: this holds a copy of the last frame
///        and reads out of it.

#include <cstdint>

#include "flight_core/types.hpp"
#include "telemetry/registry.hpp"

namespace mark4
{
    /// Exposes what the platform measured for the last frame: the raw
    /// channels, the two validity flags and the interval between frames.
    /// Fed once per frame by the composition, right after the wait point,
    /// so the values it publishes are the very ones the step consumed.
    class FrameTelemetry
    {
      public:
        /// @brief Takes the frame the loop is about to step (or just
        ///        stepped). The copy is what gives the measures an address
        ///        that outlives the loop iteration.
        /// @param frame frame to publish
        void update(const SensorFrame &frame)
        {
            if (m_seen && frame.timestampUs > m_frame.timestampUs)
            {
                m_frameDtUs = static_cast<float>(frame.timestampUs - m_frame.timestampUs);
            }
            m_frame = frame;
            m_seen = true;
        }

        /// @return the frame last published
        [[nodiscard]] const SensorFrame &frame() const
        {
            return m_frame;
        }

      private:
        /// @param context the FrameTelemetry the entry was built with
        /// @return 1 when the frame carried a fresh IMU sample, 0 otherwise
        static float ReadImuValid(const void *context)
        {
            return static_cast<const FrameTelemetry *>(context)->m_frame.imuValid ? 1.0f : 0.0f;
        }

        /// @param context the FrameTelemetry the entry was built with
        /// @return 1 when the frame carried a fresh baro sample, 0 otherwise
        static float ReadBaroValid(const void *context)
        {
            return static_cast<const FrameTelemetry *>(context)->m_frame.baroValid ? 1.0f : 0.0f;
        }

        SensorFrame m_frame;      ///< copy of the last frame handed over
        float m_frameDtUs = 0.0f; ///< interval to the frame before it [us]
        bool m_seen = false;      ///< a frame was published

        // Measures. The barometric quantity a frame carries is a pressure,
        // not an altitude: the conversion is the vertical estimator's, and
        // its result is a measure of its own (estimator/baro_altitude).
        TelemetryEntry m_gyroX{"sensor/gyro_x", TelemetryUnit::RAD_PER_S, m_frame.gyroRadS[0]};
        TelemetryEntry m_gyroY{"sensor/gyro_y", TelemetryUnit::RAD_PER_S, m_frame.gyroRadS[1]};
        TelemetryEntry m_gyroZ{"sensor/gyro_z", TelemetryUnit::RAD_PER_S, m_frame.gyroRadS[2]};
        TelemetryEntry m_accelX{"sensor/accel_x", TelemetryUnit::M_PER_S2, m_frame.accelMps2[0]};
        TelemetryEntry m_accelY{"sensor/accel_y", TelemetryUnit::M_PER_S2, m_frame.accelMps2[1]};
        TelemetryEntry m_accelZ{"sensor/accel_z", TelemetryUnit::M_PER_S2, m_frame.accelMps2[2]};
        TelemetryEntry m_baro{"sensor/baro_pressure", TelemetryUnit::PA, m_frame.baroPa};
        TelemetryEntry m_throttle{"rc/throttle", TelemetryUnit::UNITLESS, m_frame.rc.throttle};
        TelemetryEntry m_imuValid{"sensor/imu_valid", TelemetryUnit::UNITLESS, this, &ReadImuValid};
        TelemetryEntry m_baroValid{
            "sensor/baro_valid", TelemetryUnit::UNITLESS, this, &ReadBaroValid};
        TelemetryEntry m_frameDt{"sensor/frame_dt", TelemetryUnit::US, m_frameDtUs};
    };
} // namespace mark4
