#include "drone_sim_app.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flight_core/throw_detector.hpp"
#include "log/module.hpp"
#include "log_modules.hpp"
#include "ota/boot_policy.hpp"
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"
#include "transport/node_id.hpp"

namespace
{
    mark4::LogModule BOOT{mark4::LOG_MODULE_APP_BOOT, "app/boot"};
    mark4::LogModule LINK{mark4::LOG_MODULE_SIM_LINK, "sim/link"};
    mark4::LogModule FLIGHT{mark4::LOG_MODULE_FLIGHT_CORE, "flight/core"};
    mark4::LogModule OTA{mark4::LOG_MODULE_OTA_UPDATER, "ota/updater"};

    constexpr double US_PER_S = 1e6;
    constexpr long NS_PER_US = 1000L;

    /// Frames between two sim/link DEBUG lines: about one per simulated second.
    constexpr std::uint32_t LINK_DEBUG_PERIOD_FRAMES = 500U;

    /// Payload prefix of a fake image that boots but never reaches the
    /// checkpoint where a trial confirms itself (see bootFirmware).
    constexpr char OTA_BROKEN_MARKER[] = "notalive";
    constexpr std::size_t OTA_BROKEN_MARKER_SIZE = sizeof(OTA_BROKEN_MARKER) - 1U;

    /// @return human readable name of a flight phase, for the console
    const char *phaseName(mark4::FlightPhase phase)
    {
        switch (phase)
        {
            case mark4::FlightPhase::IDLE:
                return "idle";
            case mark4::FlightPhase::ALTITUDE_AUTO:
                return "altitude";
            case mark4::FlightPhase::ARMED:
                return "armed";
            case mark4::FlightPhase::BALLISTIC:
                return "ballistic";
            case mark4::FlightPhase::RECOVERY:
                return "recovery";
            case mark4::FlightPhase::HOVER:
                return "hover";
            case mark4::FlightPhase::CUTOFF:
                return "cutoff";
            case mark4::FlightPhase::MANUAL:
                return "manual";
            case mark4::FlightPhase::FAULT:
                return "fault";
            case mark4::FlightPhase::LEVEL:
                return "level";
        }
        return "?";
    }

    /// @brief Copies a directory path into an owned buffer.
    /// @param directory path to copy
    /// @return the path, truncated if it does not fit (the store then fails
    ///         to build its own file paths and says so)
    std::array<char, mark4::DroneSimApp::OTA_DIRECTORY_SIZE> makeOtaDirectory(const char *directory)
    {
        std::array<char, mark4::DroneSimApp::OTA_DIRECTORY_SIZE> path{};
        static_cast<void>(std::snprintf(path.data(), path.size(), "%s", directory));
        return path;
    }

    /// @brief Sleeps a short while, so the parked update loop polls fast
    ///        without spinning a core flat out.
    /// @param microseconds time to sleep [us]
    void sleepMicroseconds(std::uint32_t microseconds)
    {
        const timespec request = {0, static_cast<long>(microseconds) * NS_PER_US};
        static_cast<void>(::nanosleep(&request, nullptr));
    }
} // namespace

namespace mark4
{
    DroneSimApp::DroneSimApp(std::uint32_t maxFrames,
                             std::uint16_t discoveryPort,
                             std::uint32_t nodeId,
                             const char *otaDirectory)
        : m_maxFrames(maxFrames),
          m_udpLink(discoveryPort),
          m_transport(nodeId, randomBootId()),
          m_otaDirectory(makeOtaDirectory(otaDirectory))
    {
    }

    mark4_Announce DroneSimApp::Identity()
    {
        mark4_Announce self = mark4_Announce_init_zero;
        self.kind = mark4_NodeKind_DRONE_SIM;
        self.mcu = mark4_Mcu_SIM;
        self.wire_hash = WIRE_HASH;
        static_cast<void>(std::snprintf(self.name, sizeof(self.name), "%s", "drone_sim"));
        // build_epoch and git_hash stay empty: a process runs from the build
        // tree and carries no stamped image identity of its own, unlike a
        // packaged firmware image.
        return self;
    }

    std::uint64_t DroneSimApp::LogClock(void *context)
    {
        return static_cast<DroneSimApp *>(context)->m_clock.nowUs();
    }

    bool DroneSimApp::init()
    {
        logSetClock(&DroneSimApp::LogClock, this);
        static_cast<void>(logAddSink(m_consoleSink));
        if (!m_udpLink.init() || !m_transport.addLink(m_udpLink) || !m_transport.init())
        {
            BOOT.error("transport initialization failed");
            return false;
        }
        static_cast<void>(logAddSink(m_logProvider));
        BOOT.info("boot: node %08x on discovery udp/%u, wire %08x",
                  m_transport.nodeId(),
                  static_cast<unsigned>(m_udpLink.discoveryPort()),
                  WIRE_HASH);
        if (!bootFirmware())
        {
            return false;
        }
        // Last: freezing the registry means every object holding a measure
        // must already exist, and the fake bootloader above builds some.
        if (!m_telemetryProvider.init())
        {
            BOOT.error("telemetry: the registry is empty");
            return false;
        }
        BOOT.info("status: 1 message / %u frames; telemetry: %zu measures on demand",
                  static_cast<unsigned>(StatusProvider::STATUS_PERIOD_FRAMES),
                  m_telemetryProvider.entryCount());
        return true;
    }

    bool DroneSimApp::bootFirmware()
    {
        // The metadata mechanics do not depend on which slot runs, so a store
        // bound to slot A is enough to read the log and append what the
        // decision implies; the real binding follows once the slot is known.
        // init() creates whatever backing file is missing and leaves the
        // existing ones alone, which is what makes the emulated flash survive
        // the process exactly like a board's does.
        m_firmwareStore.emplace(m_otaDirectory.data(), OTA_SLOT_A);
        if (!m_firmwareStore->init())
        {
            return false; // the store logged the reason
        }

        OtaMetaState meta;
        if (!m_firmwareStore->readMeta(meta))
        {
            meta = OtaMetaState{};
        }
        const OtaBootDecision decision = otaDecideBoot(meta);
        if (decision.persist)
        {
            // Best effort, like the bootloader's: a metadata write that fails
            // must not stop something from booting, it only costs a repeated
            // trial next time.
            static_cast<void>(m_firmwareStore->writeMeta(meta));
        }

        std::uint8_t slot = decision.slot;
        if (slot != OTA_SLOT_A && slot != OTA_SLOT_B)
        {
            slot = OTA_SLOT_A;
        }
        if (!imageValidates(slot))
        {
            meta.slotState[slot] = OTA_SLOT_BAD;
            meta.trialAttempted = false;
            static_cast<void>(m_firmwareStore->writeMeta(meta));
            BOOT.warn("slot %c failed validation, falling back", slot == OTA_SLOT_B ? 'B' : 'A');
            slot = (slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
            if (!imageValidates(slot))
            {
                // A board with nothing bootable blinks and waits for SWD; a
                // process has nowhere to blink, so it says so and stops.
                BOOT.error("neither slot holds a valid image");
                return false;
            }
        }

        m_firmwareStore.emplace(m_otaDirectory.data(), slot);
        if (!m_firmwareStore->init())
        {
            return false; // the store logged the reason
        }
        // On the board a trial image confirms itself on its first ground
        // contact. The one thing this fake bootloader must be able to fake
        // is an image that never gets that far: an image whose payload
        // opens with the marker below is treated as one that boots but
        // never reaches its checkpoint, so the trial stays pending and a
        // reboot rolls it back. Real builds never carry the marker; the
        // end-to-end test writes it on purpose.
        std::array<std::uint8_t, OTA_BROKEN_MARKER_SIZE> probe{};
        const bool broken =
            m_firmwareStore->read(slot, mark4::OTA_IMAGE_HEADER_SIZE, probe.data(), probe.size()) &&
            std::memcmp(probe.data(), OTA_BROKEN_MARKER, probe.size()) == 0;
        // The handler holds a reference to the updater: gone before it, back
        // right after it.
        m_otaProvider.reset();
        m_otaUpdater.emplace(*m_firmwareStore, !broken);
        m_otaProvider.emplace(m_messenger, *m_otaUpdater, m_otaGate);
        m_otaConsumedSeen = m_otaProvider->consumed();
        refreshArmInterlock();

        BOOT.info("running slot %c, active slot %c, states %02x/%02x, emulated flash in %s",
                  slot == OTA_SLOT_B ? 'B' : 'A',
                  meta.activeSlot == OTA_SLOT_B ? 'B' : 'A',
                  static_cast<unsigned>(meta.slotState[OTA_SLOT_A]),
                  static_cast<unsigned>(meta.slotState[OTA_SLOT_B]),
                  m_otaDirectory.data());
        if (m_armInhibited)
        {
            OTA.warn("running slot on trial, arming refused until confirmed");
        }
        return true;
    }

    void DroneSimApp::rebootFirmware()
    {
        BOOT.info("reboot command: re-running the boot decision");
        if (!bootFirmware())
        {
            // Nothing bootable: the updater stops being served, which is the
            // closest a process gets to a board sitting in the bootloader.
            m_otaProvider.reset();
            m_otaUpdater.reset();
        }
        // A reset is a power cycle: the flight core starts over, tuned values
        // included, exactly like flash-less hardware.
        m_core.reset();
    }

    bool DroneSimApp::imageValidates(std::uint8_t slot) const
    {
        std::array<std::uint8_t, mark4::OTA_IMAGE_HEADER_SIZE> bytes{};
        if (!m_firmwareStore->read(slot, 0U, bytes.data(), mark4::OTA_IMAGE_HEADER_SIZE))
        {
            return false;
        }
        mark4::OtaImageHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (header.magic != mark4::OTA_IMAGE_MAGIC)
        {
            // No image was ever written here, so what runs from this slot is
            // this process's own build. This is the one place the fake
            // bootloader must differ from the real one, which always has an
            // SWD-flashed image to check; everything below is identical.
            return true;
        }
        if (header.headerVersion != mark4::OTA_IMAGE_HEADER_VERSION ||
            header.mcuId != m_firmwareStore->mcuId() || header.slotId != slot)
        {
            return false;
        }
        // The vector-table sanity check drone_boot runs has no counterpart
        // here: nothing is ever jumped to, the process is already running.
        if (header.imageCrc == mark4::OTA_IMAGE_UNSTAMPED)
        {
            // Linked but never packaged: nothing to check against, so the
            // header is trusted, exactly as on the board.
            return true;
        }
        if (header.imageSize <= mark4::OTA_IMAGE_HEADER_SIZE ||
            header.imageSize > m_firmwareStore->slotSize())
        {
            return false;
        }
        // The header CRC first: it is what makes imageSize and imageCrc
        // trustworthy enough to hash the rest of the image against.
        if (header.headerCrc !=
            m_firmwareStore->crc32(slot, 0U, offsetof(mark4::OtaImageHeader, headerCrc)))
        {
            return false;
        }
        const std::uint32_t codeSize = header.imageSize - mark4::OTA_IMAGE_HEADER_SIZE;
        return header.imageCrc ==
               m_firmwareStore->crc32(slot, mark4::OTA_IMAGE_HEADER_SIZE, codeSize);
    }

    void DroneSimApp::refreshArmInterlock()
    {
        OtaMetaState meta;
        // An unreadable metadata area says nothing about the running image,
        // and refusing to arm on a storage glitch would ground the drone for
        // a reason that has nothing to do with the firmware it runs.
        m_armInhibited = m_firmwareStore.has_value() && m_firmwareStore->readMeta(meta) &&
                         otaTrialUnconfirmed(meta, m_firmwareStore->runningSlot());
    }

    DroneSimApp::Commands::Commands(mark4::Messenger &messenger, DroneSimApp &app)
        : AbsMessageHandler(messenger, TAGS),
          m_app(app)
    {
    }

    bool DroneSimApp::Commands::onMessage(std::uint32_t src,
                                          const mark4_Envelope &envelope,
                                          std::uint64_t nowUs)
    {
        static_cast<void>(src);
        // The poll runs on the process clock; the RC fail-safe is timed on
        // the frames, so an RC packet is stamped with the last frame instead.
        static_cast<void>(nowUs);
        switch (envelope.which_body)
        {
            case mark4_Envelope_rc_tag:
                m_app.m_rcTracker.onRc(envelope.body.rc, m_app.m_lastFrameUs);
                return true;
            case mark4_Envelope_reboot_tag:
                // Acted on after the wait, by the loop that owns the reboot.
                m_app.m_rebootRequested = true;
                return true;
            case mark4_Envelope_sim_scenario_tag:
                // The plant plays it; the hash window is addressed to this
                // process and applies to the run the reset is about to open.
                m_app.m_motorSink.sendScenario(envelope.body.sim_scenario);
                m_app.m_pendingHashWindowUs = envelope.body.sim_scenario.hash_window_us;
                return true;
            default:
                return false;
        }
    }

    void DroneSimApp::runUpdateMode()
    {
        OTA.info("session open, lockstep loop parked, no actuator frames");

        // Nothing is pushed to the motor sink for as long as this loop runs,
        // so the plant gets no actuator frame and the motors are silent -
        // symmetrically with the firmware, which stops driving its ESCs.
        while (m_otaUpdater.has_value() && m_otaUpdater->sessionActive())
        {
            // Kept up while parked: the ground side must keep finding this
            // process, and an update takes longer than the keepalive period.
            // The poll dispatches, so the session is served from inside it.
            m_plantLink.poll();
            if (m_otaProvider && m_otaProvider->consumed() != m_otaConsumedSeen)
            {
                // A staging record may just have moved the running slot's
                // state, which is what the arming interlock reads.
                m_otaConsumedSeen = m_otaProvider->consumed();
                refreshArmInterlock();
            }
            m_otaUpdater->tick(m_clock.nowUs());
            if (m_rebootRequested)
            {
                break;
            }
            sleepMicroseconds(UPDATE_POLL_US);
        }

        OTA.info("session closed, resuming the lockstep loop");
        if (m_rebootRequested)
        {
            m_rebootRequested = false;
            rebootFirmware();
        }
    }

    std::uint32_t DroneSimApp::run()
    {
        mark4::SensorFrame frame;
        mark4::ActuatorFrame actuators;

        std::uint32_t steps = 0U;
        std::uint32_t announcedThrows = 0U;
        mark4::FlightPhase previousPhase = mark4::FlightPhase::IDLE;
        std::uint32_t lastResetCount = 0U;
        std::uint32_t lastSession = 0U;
        bool resetCountSeen = false;
        bool runSealed = false;
        while (m_maxFrames == 0U || steps < m_maxFrames)
        {
            if (m_otaUpdater.has_value() && m_otaUpdater->sessionActive())
            {
                // An update session suspends normal operation rather than
                // modifying it, exactly as on the board: nothing below runs
                // until the session ends.
                runUpdateMode();
                continue;
            }
            if (m_sensorSource.waitFrame(frame) != mark4::FrameWait::FRAME)
            {
                continue; // the sim source always produces a frame
            }
            m_lastFrameUs = frame.timestampUs;
            // The time base of the frames changed (the platform switched
            // between its clock and a plant's, or the plant's clock started
            // over): nothing the flight core remembers about time applies,
            // so it restarts from scratch, exactly like a power cycle on the
            // bench, and the next frame with sensors opens a new run.
            if (m_sensorSource.sessionCount() != lastSession)
            {
                lastSession = m_sensorSource.sessionCount();
                m_core.reset();
                announcedThrows = 0U;
                previousPhase = mark4::FlightPhase::IDLE;
                resetCountSeen = false;
                LINK.info("time base changed: flight core restarted");
            }
            if (frame.imuValid)
            {
                // A simulator world reset teleports the drone: no estimator
                // can (or should) track that, so the flight core restarts
                // from scratch too. Tuned parameters return to their defaults
                // with it, like flash-less hardware. Only frames with sensors
                // come from a world: the others carry no reset counter and
                // never enter a run.
                const bool worldReset =
                    resetCountSeen && m_sensorSource.resetCount() != lastResetCount;
                if (worldReset)
                {
                    m_core.reset();
                    announcedThrows = 0U;
                    previousPhase = mark4::FlightPhase::IDLE;
                    LINK.info("simulator reset at t=%.3f s: flight core restarted",
                              static_cast<double>(frame.timestampUs) / US_PER_S);
                }
                if (worldReset || !resetCountSeen)
                {
                    // A run is what happens between two resets: the hash of
                    // the previous one is finished, this one starts from
                    // scratch, on the window the scenario that opened it
                    // asked for.
                    m_runTracker.beginRun(
                        m_sensorSource.resetCount(), frame.timestampUs, m_pendingHashWindowUs);
                    runSealed = false;
                }
                lastResetCount = m_sensorSource.resetCount();
                resetCountSeen = true;
            }

            // The messages of this frame were dispatched inside the wait,
            // before the step below, so a value written from the ground is in
            // effect for the whole of the next step and never changes one
            // halfway through. What they latched is acted on here.
            if (m_rebootRequested)
            {
                m_rebootRequested = false;
                rebootFirmware();
            }
            if (m_otaProvider && m_otaProvider->consumed() != m_otaConsumedSeen)
            {
                // A staging record may just have moved the running slot's
                // state, which is what the arming interlock reads.
                m_otaConsumedSeen = m_otaProvider->consumed();
                refreshArmInterlock();
            }
            // Wall time, not the frame's: the report describes the wire,
            // which runs on the clock and not on the plant's tick grid.
            m_transportProvider.tick(m_clock.nowUs());
            if (m_otaUpdater.has_value() && m_otaUpdater->sessionActive())
            {
                // An accepted OtaBegin arrived in that wait: this frame is
                // dropped on the floor and the parked loop takes over.
                continue;
            }
            // Grafted on every frame, not only when a message arrived: the
            // frame is reused across iterations, so skipping this would leave
            // the previous iteration's RC on it and hide the fail-safe.
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

            ++steps;
            // The frame as the core is about to see it, RC graft included:
            // the platform measures publish exactly what was stepped.
            m_frameTelemetry.update(frame);
            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            if (frame.imuValid)
            {
                // The estimate and the truth of the same instant are only
                // both at hand here, which is what makes the attitude error
                // measurable at all.
                m_truthTelemetry.update(m_sensorSource.truth(), m_core.attitude());
            }
            // The plant's exact state rides next to the estimate, so a
            // consumer compares the two sample by sample; a frame without
            // sensors has no plant behind it and no truth.
            m_statusProvider.publish(frame,
                                     actuators,
                                     m_core,
                                     !m_rcTracker.failsafeActive(frame.timestampUs),
                                     frame.imuValid ? &m_sensorSource.truth() : nullptr);
            // Whatever a subscriber enabled, at the period it asked for; the
            // frame's own timestamp stamps the samples, so the service never
            // reads a clock either.
            m_telemetryProvider.sample(frame.timestampUs);
            if (frame.imuValid)
            {
                // The run is the plant's trajectory: frames without sensors
                // are not part of it and never touch the hash.
                m_runTracker.update(frame, actuators);
                m_runTracker.noteLink(m_sensorSource.lockstepTimeouts(),
                                      m_sensorSource.duplicateFrameCount());
            }
            if ((steps % LINK_DEBUG_PERIOD_FRAMES) == 0U)
            {
                LINK.debug("t=%.3f s, %u frames (%u without a plant), %u lockstep timeouts, "
                           "%u resent frames",
                           static_cast<double>(frame.timestampUs) / US_PER_S,
                           steps,
                           m_sensorSource.clockFrameCount(),
                           m_sensorSource.lockstepTimeouts(),
                           m_sensorSource.duplicateFrameCount());
            }
            if (m_runTracker.sealed() && !runSealed)
            {
                runSealed = true;
                FLIGHT.info("run %u hash %016llx over %u frames%s",
                            static_cast<unsigned>(m_runTracker.runId()),
                            static_cast<unsigned long long>(m_runTracker.hash()),
                            m_runTracker.hashedFrames(),
                            m_runTracker.degraded() ? ", LINK DEGRADED" : "");
            }
            const mark4::ThrowDetector &detector = m_core.throwDetector();
            if (detector.throwCount() > announcedThrows)
            {
                announcedThrows = detector.throwCount();
                FLIGHT.info("throw #%u detected: release %.2f m/s at t=%.3f s, "
                            "predicted apex %.2f m at t=%.3f s",
                            announcedThrows,
                            static_cast<double>(detector.releaseVelocityMps()),
                            static_cast<double>(detector.releaseTimestampUs()) / US_PER_S,
                            static_cast<double>(detector.apexAltitudeM()),
                            static_cast<double>(detector.apexTimestampUs()) / US_PER_S);
            }

            if (m_core.flightPhase() != previousPhase)
            {
                FLIGHT.info("phase %s -> %s at t=%.3f s",
                            phaseName(previousPhase),
                            phaseName(m_core.flightPhase()),
                            static_cast<double>(frame.timestampUs) / US_PER_S);
                if (m_core.flightPhase() == mark4::FlightPhase::CUTOFF)
                {
                    FLIGHT.warn("safety cutoff: motors stopped, release the arm switch and "
                                "lower the throttle to rearm");
                }
                if (m_core.flightPhase() == mark4::FlightPhase::FAULT)
                {
                    FLIGHT.error("FAULT: imu lost in flight, motors cut");
                }
                previousPhase = m_core.flightPhase();
            }
        }
        return steps;
    }

} // namespace mark4
