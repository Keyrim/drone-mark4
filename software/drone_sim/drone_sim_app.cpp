#include "drone_sim_app.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flight_core/throw_detector.hpp"
#include "platform_common/envelope_io.hpp"
#include "platform_common/ota_boot_policy.hpp"
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"

namespace
{
    constexpr double US_PER_S = 1e6;
    constexpr long NS_PER_US = 1000L;

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
          m_transport(nodeId),
          m_otaDirectory(makeOtaDirectory(otaDirectory))
    {
    }

    bool DroneSimApp::init()
    {
        if (!m_udpLink.init() || !m_transport.addLink(m_udpLink) || !m_transport.init())
        {
            static_cast<void>(std::fprintf(stderr, "drone_sim: transport initialization failed\n"));
            return false;
        }
        // The announce is the beacon: the transport broadcasts it once per
        // second and hands it to every node the moment it appears.
        mark4_Envelope announce = mark4_Envelope_init_zero;
        announce.which_body = mark4_Envelope_announce_tag;
        announce.body.announce.kind = mark4_NodeKind_DRONE_SIM;
        static_cast<void>(std::snprintf(
            announce.body.announce.name, sizeof(announce.body.announce.name), "drone_sim"));
        announce.body.announce.mcu = mark4_Mcu_SIM;
        announce.body.announce.wire_hash = WIRE_HASH;
        std::array<std::uint8_t, Transport::MAX_BEACON_SIZE> beacon{};
        std::size_t beaconSize = 0U;
        if (!encodeEnvelope(announce, beacon.data(), beacon.size(), beaconSize))
        {
            static_cast<void>(
                std::fprintf(stderr, "drone_sim: the announce does not fit a beacon\n"));
            return false;
        }
        m_transport.setBeacon(beacon.data(), beaconSize);
        return bootFirmware();
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
            std::printf("drone_sim: boot: slot %c failed validation, falling back\n",
                        slot == OTA_SLOT_B ? 'B' : 'A');
            slot = (slot == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
            if (!imageValidates(slot))
            {
                // A board with nothing bootable blinks and waits for SWD; a
                // process has nowhere to blink, so it says so and stops.
                static_cast<void>(
                    std::fprintf(stderr, "drone_sim: boot: neither slot holds a valid image\n"));
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
        m_otaUpdater.emplace(*m_firmwareStore, !broken);
        refreshArmInterlock();

        std::printf("drone_sim: boot: running slot %c, active slot %c, states %02x/%02x, "
                    "emulated flash in %s%s\n",
                    slot == OTA_SLOT_B ? 'B' : 'A',
                    meta.activeSlot == OTA_SLOT_B ? 'B' : 'A',
                    static_cast<unsigned>(meta.slotState[OTA_SLOT_A]),
                    static_cast<unsigned>(meta.slotState[OTA_SLOT_B]),
                    m_otaDirectory.data(),
                    m_armInhibited ? ", ON TRIAL: arming refused until confirmed" : "");
        static_cast<void>(std::fflush(stdout));
        return true;
    }

    void DroneSimApp::rebootFirmware()
    {
        std::printf("drone_sim: reboot command: re-running the boot decision\n");
        static_cast<void>(std::fflush(stdout));
        if (!bootFirmware())
        {
            // Nothing bootable: the updater stops being served, which is the
            // closest a process gets to a board sitting in the bootloader.
            m_otaUpdater.reset();
        }
        // A reset is a power cycle: the flight core starts over, tuned values
        // included, exactly like flash-less hardware.
        m_core = mark4::FlightCore{};
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

    bool DroneSimApp::serveOta(const mark4_Envelope &envelope, std::uint64_t nowUs)
    {
        if (!m_otaUpdater.has_value())
        {
            return false;
        }

        mark4::OtaUpdater::Inputs inputs;
        inputs.armed = m_core.armed();
        // A desktop process has no battery, so the voltage floor of
        // docs/ota-design.md section 3.2 has nothing to read here. It is not a
        // TODO: there will never be a pack behind this store.
        inputs.voltageOk = true;
        inputs.nowUs = nowUs;

        mark4_Envelope reply;
        const bool consumed = m_otaUpdater->handle(envelope, inputs, reply);
        if (reply.which_body != 0U)
        {
            // Broadcast on the telemetry route, the same one the tuning
            // answers take: the ground side reads every board-to-hub message
            // off that one stream.
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

    void DroneSimApp::runUpdateMode()
    {
        std::printf("drone_sim: ota session open, lockstep loop parked, no actuator frames\n");
        static_cast<void>(std::fflush(stdout));

        // Nothing is pushed to the motor sink for as long as this loop runs,
        // so the plant gets no actuator frame and the motors are silent -
        // symmetrically with the firmware, which stops driving its ESCs.
        std::array<std::uint8_t, MAX_PAYLOAD> command{};
        bool reboot = false;
        while (m_otaUpdater.has_value() && m_otaUpdater->sessionActive())
        {
            const std::uint64_t nowUs = m_clock.nowUs();
            // Kept up while parked: the ground side must keep finding this
            // process, and an update takes longer than the beacon period.
            m_plantLink.poll();
            for (;;)
            {
                const std::size_t size = m_commandReceiver.poll(command.data(), command.size());
                if (size == 0U)
                {
                    break;
                }
                mark4_Envelope envelope;
                if (!decodeEnvelope(command.data(), size, envelope))
                {
                    continue;
                }
                if (!serveOta(envelope, nowUs) && envelope.which_body == mark4_Envelope_reboot_tag)
                {
                    reboot = true;
                    break;
                }
            }
            m_otaUpdater->tick(m_clock.nowUs());
            if (reboot)
            {
                break;
            }
            sleepMicroseconds(UPDATE_POLL_US);
        }

        std::printf("drone_sim: ota session closed, resuming the lockstep loop\n");
        static_cast<void>(std::fflush(stdout));
        if (reboot)
        {
            rebootFirmware();
        }
    }

    void DroneSimApp::drainCommands(std::uint64_t nowUs)
    {
        m_plantLink.poll();
        std::array<std::uint8_t, MAX_PAYLOAD> command{};
        bool reboot = false;
        for (;;)
        {
            const std::size_t size = m_commandReceiver.poll(command.data(), command.size());
            if (size == 0U)
            {
                break;
            }
            mark4_Envelope envelope;
            if (!decodeEnvelope(command.data(), size, envelope))
            {
                continue;
            }
            // The updater gets first look, then each message goes to the one
            // service that owns it; whatever nobody owns (a beacon of the
            // ground side, a message this build does not know) is dropped.
            if (serveOta(envelope, m_clock.nowUs()))
            {
                continue;
            }
            switch (envelope.which_body)
            {
                case mark4_Envelope_rc_tag:
                    m_rcTracker.onRc(envelope.body.rc, nowUs);
                    break;
                case mark4_Envelope_reboot_tag:
                    // Handled after the drain, and the drain stops here: what
                    // arrived behind the reset is for the image that comes
                    // back, exactly as on a board.
                    reboot = true;
                    break;
                case mark4_Envelope_sim_scenario_tag:
                    // The plant plays it; the hash window is addressed to this
                    // process and applies to the run the reset is about to open.
                    m_motorSink.sendScenario(envelope.body.sim_scenario);
                    m_pendingHashWindowUs = envelope.body.sim_scenario.hash_window_us;
                    break;
                default:
                    static_cast<void>(m_tuningService.handle(envelope));
                    break;
            }
            if (reboot)
            {
                break;
            }
        }
        if (reboot)
        {
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
        std::uint32_t lastPlantRestarts = 0U;
        bool resetCountSeen = false;
        bool runSealed = false;
        // Starts true so the very first silent wait says "not ready" once
        bool platformReady = true;
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
                // No sensor data: the plant is gone, or not there yet. On a
                // real board a silent sensor grounds the drone but does not
                // reboot it, so this composition stays up too - it keeps
                // announcing itself and waits for the world to come back.
                if (platformReady)
                {
                    std::printf("drone_sim: platform not ready, waiting for a plant node to "
                                "send sensor frames\n");
                    static_cast<void>(std::fflush(stdout));
                    platformReady = false;
                }
                // The command path needs no world: tuning reads and writes
                // keep answering and the RC stream keeps being tracked, so
                // the bench can push coefficients to a grounded drone. Only
                // flying needs sensors. The last frame timestamp is the best
                // "now" available while the sim clock is silent.
                drainCommands(frame.timestampUs);
                continue;
            }
            if (!platformReady)
            {
                std::printf("drone_sim: platform ready, sensor frames are flowing\n");
                static_cast<void>(std::fflush(stdout));
                platformReady = true;
            }
            // A simulator world reset teleports the drone: no estimator can
            // (or should) track that, so the flight core restarts from
            // scratch, exactly like a power cycle on the bench. Tuned
            // parameters return to their defaults with it, like flash-less
            // hardware. A plant that restarted (its clock went backwards) is
            // the same event.
            const bool newPlant = m_sensorSource.plantRestarts() != lastPlantRestarts;
            if (newPlant)
            {
                std::printf("drone_sim: a new plant is driving (simulated clock restarted)\n");
                static_cast<void>(std::fflush(stdout));
            }
            lastPlantRestarts = m_sensorSource.plantRestarts();

            const bool worldReset =
                resetCountSeen && (m_sensorSource.resetCount() != lastResetCount || newPlant);
            if (worldReset)
            {
                m_core = mark4::FlightCore{};
                announcedThrows = 0U;
                previousPhase = mark4::FlightPhase::IDLE;
                std::printf("drone_sim: simulator reset at t=%.3f s: flight core restarted\n",
                            static_cast<double>(frame.timestampUs) / US_PER_S);
                static_cast<void>(std::fflush(stdout));
            }
            if (worldReset || !resetCountSeen)
            {
                // A run is what happens between two resets: the hash of the
                // previous one is finished, this one starts from scratch, on
                // the window the scenario that opened it asked for.
                m_runTracker.beginRun(
                    m_sensorSource.resetCount(), frame.timestampUs, m_pendingHashWindowUs);
                runSealed = false;
            }
            lastResetCount = m_sensorSource.resetCount();
            resetCountSeen = true;

            // Drain the command uplink before the step below, so a value
            // written from the ground is in effect for the whole of the next
            // step and never changes one halfway through.
            drainCommands(frame.timestampUs);
            if (m_otaUpdater.has_value() && m_otaUpdater->sessionActive())
            {
                // An accepted OtaBegin arrived in that drain: this frame is
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
            m_core.step(frame, actuators);
            m_motorSink.push(actuators);
            // The plant's exact state rides next to the estimate, so a ground
            // tool compares the two sample by sample.
            m_telemetryPublisher.publish(frame, actuators, m_core, &m_sensorSource.truth());
            // Paced answers to a list request: one description per frame, so
            // a table dump never bursts ahead of the telemetry it shares the
            // link with.
            m_tuningService.pump();
            m_runTracker.update(frame, actuators);
            m_runTracker.noteLink(m_sensorSource.lockstepTimeouts(),
                                  m_sensorSource.duplicateFrameCount());
            m_runTracker.publish();
            if (m_runTracker.sealed() && !runSealed)
            {
                runSealed = true;
                std::printf("drone_sim: run %u hash %016llx over %u frames%s\n",
                            static_cast<unsigned>(m_runTracker.runId()),
                            static_cast<unsigned long long>(m_runTracker.hash()),
                            m_runTracker.hashedFrames(),
                            m_runTracker.degraded() ? ", LINK DEGRADED" : "");
                static_cast<void>(std::fflush(stdout));
            }
            const mark4::ThrowDetector &detector = m_core.throwDetector();
            if (detector.throwCount() > announcedThrows)
            {
                announcedThrows = detector.throwCount();
                std::printf("drone_sim: throw #%u detected: release %.2f m/s at t=%.3f s, "
                            "predicted apex %.2f m at t=%.3f s\n",
                            announcedThrows,
                            static_cast<double>(detector.releaseVelocityMps()),
                            static_cast<double>(detector.releaseTimestampUs()) / US_PER_S,
                            static_cast<double>(detector.apexAltitudeM()),
                            static_cast<double>(detector.apexTimestampUs()) / US_PER_S);
                // Flushed so the line shows up immediately even when stdout is
                // piped (VS Code debug console, redirections): the whole point
                // is seeing the detection live.
                static_cast<void>(std::fflush(stdout));
            }

            if (m_core.flightPhase() != previousPhase)
            {
                std::printf("drone_sim: phase %s -> %s at t=%.3f s\n",
                            phaseName(previousPhase),
                            phaseName(m_core.flightPhase()),
                            static_cast<double>(frame.timestampUs) / US_PER_S);
                if (m_core.flightPhase() == mark4::FlightPhase::CUTOFF)
                {
                    std::printf("drone_sim: safety cutoff: motors stopped, release the "
                                "arm switch and lower the throttle to rearm\n");
                }
                // Flushed so the line shows up immediately even when stdout is
                // piped (VS Code debug console, redirections).
                static_cast<void>(std::fflush(stdout));
                previousPhase = m_core.flightPhase();
            }
        }
        return steps;
    }

} // namespace mark4
