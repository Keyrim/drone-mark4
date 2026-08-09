#include "firmware_app.hpp"

#include <cstdint>
#include <cstring>

#include "flight_core/types.hpp"
#include "platform_stm32/board.hpp"
#include "platform_stm32/rtt.hpp"
#include "platform_stm32/uart1.hpp"
#include "protocol/commands.hpp"
#include "protocol/serial_framing.hpp"
#include "protocol/version.hpp"
#include "status_leds.hpp"

namespace
{
    constexpr std::uint32_t US_PER_S = 1000000U;
    constexpr std::uint32_t US_PER_MS = 1000U;

    /// OSR-4096 conversion time of the MS5611, datasheet maximum.
    constexpr std::uint32_t MS5611_CONVERSION_US = 9040U;

    /// The barometer wait states must outlast its 9.04 ms OSR-4096
    /// conversion at the loop rate the state machine is pumped at.
    constexpr std::uint32_t FRAME_PERIOD_US = US_PER_S / mark4::SensorSourceStm32::FRAME_RATE_HZ;
    static_assert(mark4::Ms5611::CONVERSION_WAIT_UPDATES * FRAME_PERIOD_US >= MS5611_CONVERSION_US,
                  "MS5611 conversion outlasts the wait budget");

    static_assert(mark4::LED_FRAMES_PER_SLOT * mark4::LED_PATTERN_SLOTS ==
                      mark4::SensorSourceStm32::FRAME_RATE_HZ,
                  "the LED pattern cycle must span exactly one second");

    /// Scale from a unit quantity to its milli multiple.
    constexpr float MILLI_PER_UNIT = 1000.0f;

    /// @brief Millis of a float for integer-only printf: "%d.%03d".
    /// @param value converted value
    /// @return value scaled by 1000, rounded toward zero
    long milli(float value)
    {
        return static_cast<long>(value * MILLI_PER_UNIT);
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

        m_clock.init();
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
        m_sensorSource.init();
        if (!m_telemetrySender.init())
        {
            rttWrite("telemetry: uart init failed\n");
            return false;
        }
        if (!m_commandReceiver.init())
        {
            rttWrite("rc: uart init failed\n");
            return false;
        }
        rttPrintf("loop: %lu Hz, timer paced; telemetry: %lu baud, 1 packet / %lu frames; "
                  "blackbox: every frame, same link; rc uplink armed with %lu ms fail-safe\n",
                  static_cast<unsigned long>(SensorSourceStm32::FRAME_RATE_HZ),
                  static_cast<unsigned long>(UART1_BAUD_RATE),
                  static_cast<unsigned long>(TelemetryPublisher::DECIMATION),
                  static_cast<unsigned long>(RC_TIMEOUT_US / US_PER_MS));
        return true;
    }

    void FirmwareApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        std::uint32_t frames = 0U;
        std::uint64_t lastStatusUs = 0U;
        std::uint32_t lastFailureCount = 0U;
        bool degraded = false;

        // Last pilot state seen on the uplink, safe until proven fresh:
        // the SensorFrame defaults (kill engaged, disarmed) apply before
        // the first packet and whenever the fail-safe trips.
        mark4::RcInput rc;
        std::uint64_t lastRcUs = 0U;
        bool rcEverReceived = false;
        for (;;)
        {
            if (m_sensorSource.waitFrame(frame) != FrameWait::FRAME)
            {
                continue; // the timer-paced source only ever produces FRAME
            }

            std::uint8_t packet[SERIAL_MAX_PAYLOAD];
            for (;;)
            {
                const std::size_t size = m_commandReceiver.poll(packet, sizeof(packet));
                if (size == 0U)
                {
                    break;
                }
                if (size == RC_COMMAND_PACKET_SIZE && packet[0] == PROTOCOL_VERSION)
                {
                    RcCommandPacket command{};
                    std::memcpy(&command, packet, sizeof(command));
                    rc.killSwitch = command.killSwitch != 0U;
                    rc.armSwitch = command.armSwitch != 0U;
                    rc.throttle = command.throttle;
                    lastRcUs = frame.timestampUs;
                    rcEverReceived = true;
                }
                else if (size == REBOOT_COMMAND_PACKET_SIZE && packet[0] == PROTOCOL_VERSION &&
                         packet[1] == BOARD_REBOOT_MAGIC)
                {
                    rttWrite("rc: reboot command, resetting\n");
                    systemReset();
                }
            }
            const bool rcLost = !rcEverReceived || (frame.timestampUs - lastRcUs) > RC_TIMEOUT_US;
            frame.rc = rcLost ? mark4::RcInput{} : rc;

            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            m_blackbox.record(frame, actuators);
            updateStatusLeds(m_core.flightPhase(), frame.rc.killSwitch, degraded, frames);

            ++frames;
            m_telemetryPublisher.publish(frame, actuators, m_core);
            if ((frames % FRAMES_PER_STATUS) == 0U)
            {
                // A health counter that moved during the last window keeps
                // LED1 on the degraded pattern for the next one.
                const std::uint32_t failureCount =
                    m_sensorSource.overruns() + m_sensorSource.readFailures() + m_baro.failures() +
                    m_telemetrySender.packetsDropped();
                degraded = failureCount != lastFailureCount;
                lastFailureCount = failureCount;
                const std::uint64_t nowUs = frame.timestampUs;
                const auto periodUs =
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
                rttPrintf("tx: %lu sent %lu dropped  rc: %lu received%s  phase %u\n",
                          static_cast<unsigned long>(m_telemetrySender.packetsSent()),
                          static_cast<unsigned long>(m_telemetrySender.packetsDropped()),
                          static_cast<unsigned long>(m_commandReceiver.packetsReceived()),
                          rcLost ? " (failsafe)" : "",
                          static_cast<unsigned>(m_core.flightPhase()));
            }
        }
    }
} // namespace mark4
