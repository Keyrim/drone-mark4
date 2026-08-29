#include "firmware_app.hpp"

#include <cstdint>

#include "flight_core/types.hpp"
#include "platform_common/ota_boot_policy.hpp"
#include "platform_stm32/board.hpp"
#include "platform_stm32/rtt.hpp"
#include "platform_stm32/uart1.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"
#include "protocol/ota.hpp"
#include "protocol/serial_framing.hpp"
#include "status_leds.hpp"

namespace
{
    constexpr std::uint32_t US_PER_MS = 1000U;

    /// The barometer runs on its own free-running grid, so the loop cannot
    /// line up with it and can only oversample it. Reading at twice the
    /// output rate keeps the pressure in a frame younger than one output
    /// period even with the rate tolerance of both clocks; at parity, a
    /// slow tick would let a whole output go unread.
    constexpr std::uint32_t BARO_READ_RATE_HZ =
        mark4::SensorSourceStm32::FRAME_RATE_HZ / mark4::Bmp581::TICKS_PER_READ;
    static_assert(BARO_READ_RATE_HZ >= 2U * mark4::Bmp581::OUTPUT_RATE_HZ,
                  "the barometer must be read at least twice per output");

    static_assert(mark4::LED_FRAMES_PER_SLOT * mark4::LED_PATTERN_SLOTS ==
                      mark4::SensorSourceStm32::FRAME_RATE_HZ,
                  "the LED pattern cycle must span exactly one second");

    /// Scale from a unit quantity to its milli multiple.
    constexpr float MILLI_PER_UNIT = 1000.0f;

    /// Room for the largest answer the updater emits, which is the status
    /// packet; the chunk acknowledgement and the single ack packet are both
    /// shorter.
    constexpr std::size_t OTA_REPLY_SIZE = mark4::OTA_STATUS_PACKET_SIZE;
    static_assert(OTA_REPLY_SIZE >= mark4::OTA_CHUNK_ACK_PACKET_SIZE &&
                      OTA_REPLY_SIZE >= mark4::OTA_ACK_PACKET_SIZE,
                  "every updater answer must fit the reply buffer");

    /// The whole chunk packet must fit one serial frame payload, which is
    /// what the command receiver hands out at most.
    static_assert(mark4::OTA_CHUNK_PACKET_SIZE <= mark4::SERIAL_MAX_PAYLOAD,
                  "a chunk must survive the framing on the way in");

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
            // Not fatal: the altitude channel is one input among several,
            // and a board that refuses to boot over it says nothing at all
            // on the link it would have been diagnosed from. The driver
            // logged the reason, the frames carry baroPa = 0.
            rttWrite("baro: init failed, flying without the pressure channel\n");
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
                  "rc uplink armed with %lu ms fail-safe\n",
                  static_cast<unsigned long>(SensorSourceStm32::FRAME_RATE_HZ),
                  static_cast<unsigned long>(UART1_BAUD_RATE),
                  static_cast<unsigned long>(TelemetryPublisher::DECIMATION),
                  static_cast<unsigned long>(RcTracker::RC_TIMEOUT_US / US_PER_MS));

        refreshArmInterlock();
        rttPrintf("ota: running slot %c, %lu byte slots%s\n",
                  m_firmwareStore.runningSlot() == OTA_SLOT_B ? 'B' : 'A',
                  static_cast<unsigned long>(m_firmwareStore.slotSize()),
                  m_armInhibited ? ", ON TRIAL: arming refused until confirmed" : "");
        return true;
    }

    void FirmwareApp::refreshArmInterlock()
    {
        OtaMetaState meta;
        // An unreadable metadata area says nothing about the running image,
        // and refusing to arm on a storage glitch would ground the drone for
        // a reason that has nothing to do with the firmware it runs.
        m_armInhibited = m_firmwareStore.readMeta(meta) &&
                         otaTrialUnconfirmed(meta, m_firmwareStore.runningSlot());
    }

    bool FirmwareApp::IsRebootCommand(const std::uint8_t *packet, std::size_t size)
    {
        return size == REBOOT_COMMAND_PACKET_SIZE &&
               hasHeader(packet, size, PacketType::REBOOT_COMMAND) &&
               packet[2] == BOARD_REBOOT_MAGIC;
    }

    bool FirmwareApp::serveOta(const std::uint8_t *packet, std::size_t size, std::uint64_t nowUs)
    {
        OtaUpdater::Inputs inputs;
        inputs.armed = m_core.armed();
        // TODO(tmagne): read the real pack voltage here. mark1 has no battery
        // sense at all, so the voltage floor of docs/ota-design.md section 3.2
        // cannot be enforced yet; the AIO board brings the divider that makes
        // it measurable.
        inputs.voltageOk = true;
        inputs.nowUs = nowUs;

        std::uint8_t reply[OTA_REPLY_SIZE];
        bool consumed = false;
        const std::size_t replySize =
            m_otaUpdater.handle(packet, size, inputs, reply, sizeof(reply), consumed);
        if (replySize != 0U)
        {
            // The same UART telemetry and the tuning answers go out by: OTA
            // is one more packet type on the one link this board has.
            m_telemetrySender.send(reply, replySize);
        }
        if (consumed)
        {
            // A confirm or a staging record may just have moved the running
            // slot's state, which is what the arming interlock reads.
            refreshArmInterlock();
        }
        return consumed;
    }

    void FirmwareApp::runUpdateMode()
    {
        rttWrite("ota: session open, flight loop parked, no motor output\n");

        // Nothing pushes the motor sink for as long as this loop runs, so the
        // ESCs observe silence and disarm: update mode does not modify the
        // kill-switch semantics of normal operation, it suspends normal
        // operation entirely. RC is deliberately ignored too - the fail-safe
        // is already the safe state, and it will have engaged by the time the
        // flight loop resumes.
        std::uint8_t packet[SERIAL_MAX_PAYLOAD];
        while (m_otaUpdater.sessionActive())
        {
            const std::uint64_t nowUs = m_clock.nowUs();
            for (;;)
            {
                const std::size_t size = m_commandReceiver.poll(packet, sizeof(packet));
                if (size == 0U)
                {
                    break;
                }
                if (!serveOta(packet, size, nowUs) && IsRebootCommand(packet, size))
                {
                    rttWrite("ota: reboot command during a session, resetting\n");
                    systemReset();
                }
            }
            m_otaUpdater.tick(m_clock.nowUs());
            // TODO(tmagne): refresh the independent watchdog here, and before
            // each sector erase inside the store, once the firmware starts one
            // at all. There is no watchdog today, so an update that wedges the
            // core needs a power cycle rather than costing the trial attempt.
        }

        rttPrintf("ota: session closed, resuming the flight loop (rx drops %u)\n",
                  static_cast<unsigned>(uart1RxDrops()));
    }

    void FirmwareApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        std::uint32_t frames = 0U;
        std::uint64_t lastStatusUs = 0U;
        std::uint32_t lastFailureCount = 0U;
        bool degraded = false;

        for (;;)
        {
            if (m_sensorSource.waitFrame(frame) != FrameWait::FRAME)
            {
                continue; // the timer-paced source only ever produces FRAME
            }

            // The tracker consumes the RC packets and hands back everything
            // else, which this composition answers itself.
            std::uint8_t packet[SERIAL_MAX_PAYLOAD];
            for (;;)
            {
                const std::size_t size =
                    m_rcTracker.pump(packet, sizeof(packet), frame.timestampUs);
                if (size == 0U)
                {
                    break;
                }
                if (serveOta(packet, size, frame.timestampUs))
                {
                    continue; // the updater claimed it, whatever it answered
                }
                if (IsRebootCommand(packet, size))
                {
                    rttWrite("rc: reboot command, resetting\n");
                    systemReset();
                }
                else
                {
                    // Answered here, before the step below, so a value
                    // written from the bench is in effect for the whole of
                    // the next step and never changes one halfway through.
                    static_cast<void>(m_tuningService.handle(packet, size));
                }
            }
            // An accepted OTA_BEGIN parks everything below until the session
            // ends: this frame is dropped on the floor, which is exactly what
            // suspending normal operation means.
            if (m_otaUpdater.sessionActive())
            {
                runUpdateMode();
                continue;
            }
            m_rcTracker.graft(frame);
            if (m_armInhibited)
            {
                // The running image has not proven its link yet, so it may
                // not take the drone into the air. Clearing the arm switch is
                // the whole interlock: the flight core's single arming gate
                // is that field, and it stays the one place arming is
                // decided (docs/ota-design.md section 3.2).
                frame.rc.armSwitch = false;
            }

            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            updateStatusLeds(m_core.flightPhase(), frame.rc.killSwitch, degraded, frames);

            ++frames;
            m_telemetryPublisher.publish(frame, actuators, m_core);
            // Paced answers to a list request: one description per frame, so
            // a table dump never bursts ahead of the telemetry sharing the
            // same UART.
            m_tuningService.pump();
            if ((frames % FRAMES_PER_STATUS) == 0U)
            {
                // A health counter that moved during the last window keeps
                // LED1 on the degraded pattern for the next one.
                const std::uint32_t failureCount =
                    m_sensorSource.overruns() + m_sensorSource.readFailures() + m_baro.failures() +
                    m_baro.implausibleSolutions() + m_telemetrySender.packetsDropped();
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
                rttPrintf("tx: %lu sent %lu dropped  rc: %lu received%s  "
                          "tuning: %lu asked %lu answered  phase %u\n",
                          static_cast<unsigned long>(m_telemetrySender.packetsSent()),
                          static_cast<unsigned long>(m_telemetrySender.packetsDropped()),
                          static_cast<unsigned long>(m_commandReceiver.packetsReceived()),
                          m_rcTracker.failsafeActive(frame.timestampUs) ? " (failsafe)" : "",
                          static_cast<unsigned long>(m_tuningService.requestCount()),
                          static_cast<unsigned long>(m_tuningService.answerCount()),
                          static_cast<unsigned>(m_core.flightPhase()));
            }
        }
    }
} // namespace mark4
