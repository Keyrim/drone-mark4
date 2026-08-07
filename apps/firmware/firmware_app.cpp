#include "firmware_app.hpp"

#include <cstdint>

#include "flight_core/types.hpp"
#include "platform_stm32/board.hpp"
#include "platform_stm32/rtt.hpp"

namespace
{
    /// The barometer wait states must outlast its 9.04 ms OSR-4096
    /// conversion at the loop rate the state machine is pumped at.
    constexpr std::uint32_t FRAME_PERIOD_US = 1000000U / mark4::SensorSourceStm32::FRAME_RATE_HZ;
    static_assert(mark4::Ms5611::CONVERSION_WAIT_UPDATES * FRAME_PERIOD_US >= 9040U,
                  "MS5611 conversion outlasts the wait budget");

    /// @brief Millis of a float for integer-only printf: "%d.%03d".
    /// @param value converted value
    /// @return value scaled by 1000, rounded toward zero
    long milli(float value)
    {
        return static_cast<long>(value * 1000.0f);
    }
} // namespace

namespace mark4
{
    bool FirmwareApp::init()
    {
        const bool clockOk = initSystemClock();
        initCycleCounter();
        rttInit();
        rttWrite("\nmark4 firmware\n");
        if (!clockOk)
        {
            rttWrite("clock: HSE or PLL never ready, staying on HSI\n");
            return false;
        }
        rttPrintf("clock: %lu Hz\n", static_cast<unsigned long>(coreClockHz()));
        initLeds();

        if (!m_bus.init())
        {
            rttWrite("i2c1: init failed, bus stuck busy\n");
            return false;
        }
        if (!m_imu.init())
        {
            return false; // the driver logged the reason
        }
        if (!m_baro.init())
        {
            return false; // the driver logged the reason
        }
        m_clock.init();
        m_sensorSource.init();
        rttPrintf("loop: %lu Hz, timer paced\n",
                  static_cast<unsigned long>(SensorSourceStm32::FRAME_RATE_HZ));
        return true;
    }

    void FirmwareApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        std::uint32_t frames = 0U;
        std::uint64_t lastStatusUs = 0U;
        for (;;)
        {
            if (!m_sensorSource.waitFrame(frame))
            {
                continue;
            }
            m_core.step(frame, actuators);
            m_motorSink.push(actuators);

            ++frames;
            if ((frames % FRAMES_PER_LED_TOGGLE) == 0U)
            {
                toggleLed1();
            }
            if ((frames % FRAMES_PER_STATUS) == 0U)
            {
                const std::uint64_t nowUs = frame.timestampUs;
                const std::uint32_t periodUs =
                    static_cast<std::uint32_t>((nowUs - lastStatusUs) / FRAMES_PER_STATUS);
                lastStatusUs = nowUs;
                rttPrintf("t %lu us/frame  gyro %ld %ld %ld mrad/s  acc %ld %ld %ld mm/s2  "
                          "baro %ld Pa  motors %ld %ld %ld %ld  ovr %lu err %lu/%lu\n",
                          static_cast<unsigned long>(periodUs),
                          milli(frame.gyroRadS[0]),
                          milli(frame.gyroRadS[1]),
                          milli(frame.gyroRadS[2]),
                          milli(frame.accelMps2[0]),
                          milli(frame.accelMps2[1]),
                          milli(frame.accelMps2[2]),
                          static_cast<long>(frame.baroPa),
                          milli(m_motorSink.last().motor[0]),
                          milli(m_motorSink.last().motor[1]),
                          milli(m_motorSink.last().motor[2]),
                          milli(m_motorSink.last().motor[3]),
                          static_cast<unsigned long>(m_sensorSource.overruns()),
                          static_cast<unsigned long>(m_sensorSource.readFailures()),
                          static_cast<unsigned long>(m_baro.failures()));
            }
        }
    }
} // namespace mark4
