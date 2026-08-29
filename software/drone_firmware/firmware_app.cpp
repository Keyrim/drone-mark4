#include "firmware_app.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flight_core/types.hpp"
#include "platform_common/envelope_io.hpp"
#include "platform_common/ota_boot_policy.hpp"
#include "platform_stm32/rtt.hpp"
#include "platform_stm32/uart1.hpp"
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"
#include "status_leds.hpp"
#include "transport/serial_framing.hpp"

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

    /// Every Envelope must survive the transport header and the framing on
    /// the way in and out.
    static_assert(mark4::FRAME_HEADER_SIZE + mark4::MAX_ENVELOPE_SIZE <= mark4::SERIAL_MAX_PAYLOAD,
                  "an Envelope behind the transport header must fit one serial frame");

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
    void FirmwareApp::log(mark4_LogLevel level, const char *text, std::uint64_t nowUs)
    {
        rttWrite(text);
        rttWrite("\n");
        m_log.log(level, text, nowUs);
    }

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
        // The link first: from here on a failure is told to the bench too,
        // which has no J-Link when the board is updated over the air. The
        // bytes sit in the transmit ring and the interrupt drains them
        // whatever main() does next.
        if (!uart1Init() || !m_transport.addLink(m_uartLink) || !m_transport.init())
        {
            rttWrite("transport: uart init failed\n");
            return false;
        }
        if (!setAnnounceBeacon())
        {
            log(mark4_LogLevel_ERROR, "transport: the announce does not fit a beacon", 0U);
            return false;
        }
        rttPrintf("transport: node %08lx, %lu baud\n",
                  static_cast<unsigned long>(m_transport.nodeId()),
                  static_cast<unsigned long>(UART1_BAUD_RATE));
        if (!m_bus.init())
        {
            log(mark4_LogLevel_ERROR, "i2c1: init failed, bus stuck busy", m_clock.nowUs());
            return false;
        }
        if (!m_imu.init())
        {
            log(mark4_LogLevel_ERROR, "imu: init failed", m_clock.nowUs()); // RTT has the reason
            return false;
        }
        if (!m_baro.init())
        {
            // Not fatal: the altitude channel is one input among several,
            // and a board that refuses to boot over it says nothing at all
            // on the link it would have been diagnosed from. The driver
            // logged the reason, the frames carry baroPa = 0.
            log(mark4_LogLevel_WARN,
                "baro: init failed, flying without the pressure channel",
                m_clock.nowUs());
        }
        m_sensorSource.init();
        rttPrintf("loop: %lu Hz, timer paced; telemetry: 1 message / %lu frames; "
                  "rc uplink armed with %lu ms fail-safe\n",
                  static_cast<unsigned long>(SensorSourceStm32::FRAME_RATE_HZ),
                  static_cast<unsigned long>(TelemetryPublisher::DECIMATION),
                  static_cast<unsigned long>(RcTracker::RC_TIMEOUT_US / US_PER_MS));

        refreshArmInterlock();
        rttPrintf("ota: running slot %c, %lu byte slots%s\n",
                  m_firmwareStore.runningSlot() == OTA_SLOT_B ? 'B' : 'A',
                  static_cast<unsigned long>(m_firmwareStore.slotSize()),
                  m_armInhibited ? ", ON TRIAL: arming refused until confirmed" : "");
        if (m_armInhibited)
        {
            log(mark4_LogLevel_WARN,
                "ota: running slot on trial, arming refused until confirmed",
                m_clock.nowUs());
        }
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

    bool FirmwareApp::setAnnounceBeacon()
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_announce_tag;
        mark4_Announce &announce = envelope.body.announce;
        announce.kind = mark4_NodeKind_FIRMWARE;
        std::strncpy(announce.name, "mark4-fc", sizeof(announce.name) - 1U);
        announce.mcu = static_cast<mark4_Mcu>(m_firmwareStore.mcuId());
        announce.wire_hash = WIRE_HASH;
        // The identity is whatever the packaging script stamped into this
        // slot's image header: unstamped (SWD-flashed) images carry erased
        // bytes, reported as 0xFFFFFFFF and an empty hash.
        std::array<std::uint8_t, OtaUpdater::HEADER_PREFIX_SIZE> bytes{};
        if (m_firmwareStore.read(m_firmwareStore.runningSlot(), 0U, bytes.data(), bytes.size()))
        {
            OtaImageHeader header{};
            std::memcpy(&header, bytes.data(), bytes.size());
            if (header.magic == OTA_IMAGE_MAGIC)
            {
                announce.build_epoch = header.buildEpoch;
                std::array<char, OTA_GIT_HASH_SIZE> hash{};
                std::memcpy(hash.data(), &header.gitHash, OTA_GIT_HASH_SIZE);
                otaGitHashToWire(hash, announce.git_hash);
            }
        }
        // The announce is the beacon: the transport broadcasts it once per
        // second and unicasts it to every node the moment it appears.
        std::array<std::uint8_t, Transport::MAX_BEACON_SIZE> beacon{};
        std::size_t beaconSize = 0U;
        if (!encodeEnvelope(envelope, beacon.data(), beacon.size(), beaconSize))
        {
            return false;
        }
        m_transport.setBeacon(beacon.data(), beaconSize);
        return true;
    }

    void FirmwareApp::pollTransport(std::uint64_t nowUs)
    {
        m_transport.poll(nowUs, &FirmwareApp::OnPayload, this);
    }

    void FirmwareApp::OnPayload(void *context,
                                std::uint32_t src,
                                const std::uint8_t *payload,
                                std::size_t size)
    {
        static_cast<void>(src); // every answer is a broadcast, nobody is addressed
        static_cast<FirmwareApp *>(context)->m_commandReceiver.push(payload, size);
    }

    bool FirmwareApp::serveOta(const mark4_Envelope &envelope, std::uint64_t nowUs)
    {
        OtaUpdater::Inputs inputs;
        inputs.armed = m_core.armed();
        // TODO(tmagne): read the real pack voltage here. mark1 has no battery
        // sense at all, so the voltage floor of docs/ota-design.md section 3.2
        // cannot be enforced yet; the AIO board brings the divider that makes
        // it measurable.
        inputs.voltageOk = true;
        inputs.nowUs = nowUs;

        mark4_Envelope reply;
        const bool consumed = m_otaUpdater.handle(envelope, inputs, reply);
        if (reply.which_body != 0U)
        {
            // The same path telemetry and the tuning answers go out by: the
            // updater is one more message type on the one link this board has.
            static_cast<void>(sendEnvelope(m_telemetrySender, reply));
        }
        if (consumed)
        {
            // A staging record may just have moved the running slot's state,
            // which is what the arming interlock reads.
            refreshArmInterlock();
        }
        return consumed;
    }

    bool FirmwareApp::drainCommands(std::uint64_t nowUs)
    {
        std::uint8_t packet[MAX_PAYLOAD];
        for (;;)
        {
            const std::size_t size = m_commandReceiver.poll(packet, sizeof(packet));
            if (size == 0U)
            {
                return false;
            }
            mark4_Envelope envelope;
            if (!decodeEnvelope(packet, size, envelope))
            {
                continue;
            }
            if (serveOta(envelope, nowUs))
            {
                continue; // the updater claimed it, whatever it answered
            }
            switch (envelope.which_body)
            {
                case mark4_Envelope_rc_tag:
                    m_rcTracker.onRc(envelope.body.rc, nowUs);
                    break;
                case mark4_Envelope_reboot_tag:
                    return true;
                default:
                    // Answered here, before the step, so a value written from
                    // the bench is in effect for the whole of the next step
                    // and never changes one halfway through. Anything else
                    // (another node's telemetry relayed onto this link) is
                    // not a request and is ignored there.
                    static_cast<void>(m_tuningService.handle(envelope));
                    break;
            }
        }
    }

    void FirmwareApp::runUpdateMode()
    {
        log(mark4_LogLevel_INFO,
            "ota: session open, flight loop parked, no motor output",
            m_clock.nowUs());

        // Nothing pushes the motor sink for as long as this loop runs, so the
        // ESCs observe silence and disarm: update mode does not modify the
        // kill-switch semantics of normal operation, it suspends normal
        // operation entirely. RC is deliberately ignored too - the fail-safe
        // is already the safe state, and it will have engaged by the time the
        // flight loop resumes.
        while (m_otaUpdater.sessionActive())
        {
            const std::uint64_t nowUs = m_clock.nowUs();
            pollTransport(nowUs);
            if (drainCommands(nowUs))
            {
                log(mark4_LogLevel_WARN, "ota: reboot command during a session, resetting", nowUs);
                systemReset();
            }
            m_otaUpdater.tick(m_clock.nowUs());
            // TODO(tmagne): refresh the independent watchdog here, and before
            // each sector erase inside the store, once the firmware starts one
            // at all. There is no watchdog today, so an update that wedges the
            // core needs a power cycle rather than costing the trial attempt.
        }

        char line[RTT_PRINTF_SIZE];
        static_cast<void>(std::snprintf(line,
                                        sizeof(line),
                                        "ota: session closed, resuming the flight loop "
                                        "(rx drops %lu)",
                                        static_cast<unsigned long>(uart1RxDrops())));
        log(mark4_LogLevel_INFO, line, m_clock.nowUs());
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

            // Once per frame, right after the wait: the frame's timestamp is
            // the loop's instant, and the transport never reads a clock.
            pollTransport(frame.timestampUs);
            if (drainCommands(frame.timestampUs))
            {
                log(mark4_LogLevel_WARN, "rc: reboot command, resetting", frame.timestampUs);
                systemReset();
            }
            // An accepted OtaBegin parks everything below until the session
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
                    m_baro.implausibleSolutions() + m_telemetrySender.dropCount();
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
                rttPrintf("tx: %lu sent %lu dropped  rx: %lu received, %lu nodes%s  "
                          "tuning: %lu asked %lu answered  phase %u\n",
                          static_cast<unsigned long>(m_telemetrySender.packetCount()),
                          static_cast<unsigned long>(m_telemetrySender.dropCount()),
                          static_cast<unsigned long>(m_commandReceiver.packetsReceived()),
                          static_cast<unsigned long>(m_transport.nodeCount()),
                          m_rcTracker.failsafeActive(frame.timestampUs) ? " (failsafe)" : "",
                          static_cast<unsigned long>(m_tuningService.requestCount()),
                          static_cast<unsigned long>(m_tuningService.answerCount()),
                          static_cast<unsigned>(m_core.flightPhase()));
            }
        }
    }
} // namespace mark4
