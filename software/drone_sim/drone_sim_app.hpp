#pragma once

/// @file
/// @brief drone_sim composition root.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "flight_core/flight_core.hpp"
#include "platform_common/announce_publisher.hpp"
#include "platform_common/blackbox.hpp"
#include "platform_common/ota_updater.hpp"
#include "platform_common/rc_tracker.hpp"
#include "platform_common/telemetry_publisher.hpp"
#include "platform_common/tuning_service.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/command_receiver_sim.hpp"
#include "platform_sim/firmware_store_sim.hpp"
#include "platform_sim/log_sink_file.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/sim_run_tracker.hpp"
#include "platform_sim/telemetry_sender_sim.hpp"
#include "platform_sim/udp_link.hpp"
#include "protocol/header.hpp"

namespace mark4
{
    /// Composition root: owns every service and the flight core as value members.
    /// Member declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no manual
    /// teardown. Built by main(), passed by reference: no singleton.
    class DroneSimApp
    {
      public:
        /// Granularity of the sensor wait [ms]. A silent plant does not end
        /// the run - the platform is just not ready, like a dead sensor on a
        /// real board - but the loop must keep waking up to announce itself,
        /// and the announce contract is one per second.
        static constexpr std::uint32_t IDLE_TIMEOUT_MS = 500U;

        /// Directory the blackbox file is created in, relative to the cwd.
        static constexpr const char *LOG_DIRECTORY = "logs/blackbox";

        /// Size of the buffer holding the timestamped blackbox file path.
        static constexpr std::size_t LOG_PATH_SIZE = 64U;

        /// Size of the buffer holding the emulated-flash directory. The
        /// store builds its own file paths inside it, so this only has to
        /// hold the directory itself.
        static constexpr std::size_t OTA_DIRECTORY_SIZE = 192U;

        /// Poll period of the parked update loop [us]. Short enough that the
        /// sender's chunk pacing is never the thing waiting, long enough that
        /// a transfer does not spin a core flat out.
        static constexpr std::uint32_t UPDATE_POLL_US = 500U;

        /// @param maxFrames number of frames to process before stopping,
        ///        0 = no limit (the run ends with the operator or the link)
        /// @param simPort UDP port the sim link listens on
        /// @param telemetryPort UDP port telemetry is broadcast to
        /// @param rcPort UDP port the command receiver binds, for the RC
        ///        stream the pilot keeps up
        /// @param sessionId identity of this process start, announced so the
        ///        ground side tells a restart from a refresh
        /// @param otaDirectory directory holding the emulated flash slots and
        ///        boot metadata; copied, so the caller keeps its buffer
        explicit DroneSimApp(std::uint32_t maxFrames,
                             std::uint16_t simPort,
                             std::uint16_t telemetryPort,
                             std::uint16_t rcPort,
                             std::uint32_t sessionId,
                             const char *otaDirectory);

        /// @brief Initializes services in declaration order: binds the sim link,
        ///        opens the telemetry socket and the blackbox file, then runs
        ///        the fake bootloader that picks the firmware slot. The first
        ///        failure is logged by the service and returns false immediately.
        /// @return true when every service is ready
        bool init();

        /// @brief Runs the waitFrame -> step -> push -> record loop until the
        ///        requested number of frames is reached or the sim link goes idle.
        /// @return number of steps executed
        std::uint32_t run();

        /// @return telemetry service, for post-run reporting
        [[nodiscard]] const mark4::TelemetrySenderSim &accessTelemetrySender() const
        {
            return m_telemetrySender;
        }

        /// @return motor sink, for post-run reporting
        [[nodiscard]] const mark4::MotorSinkSim &accessMotorSink() const
        {
            return m_motorSink;
        }

        /// @return clock service
        [[nodiscard]] mark4::AbsClock &accessClock()
        {
            return m_clock;
        }

        /// @return blackbox log sink, for post-run reporting
        [[nodiscard]] const mark4::LogSinkFile &accessLogSink() const
        {
            return m_logSink;
        }

        /// @return blackbox recorder, for post-run reporting
        [[nodiscard]] const mark4::Blackbox &accessBlackbox() const
        {
            return m_blackbox;
        }

        /// @return sensor source, for post-run reporting
        [[nodiscard]] const mark4::SensorSourceSim &accessSensorSource() const
        {
            return m_sensorSource;
        }

        /// @return run tracker, for post-run reporting
        [[nodiscard]] const mark4::SimRunTracker &accessRunTracker() const
        {
            return m_runTracker;
        }

      private:
        /// @brief The fake bootloader: runs the slot decision shared with
        ///        drone_boot (platform_common/ota_boot_policy.hpp) over the
        ///        file-backed metadata, validates the slot it picked the way
        ///        the bootloader validates an image, then binds the store and
        ///        the updater to the slot that won. Called at init and on
        ///        every reboot command, which is what makes a hub-driven
        ///        update against this process exercise the trial boot and the
        ///        rollback with no hardware at all.
        /// @return true when a slot is running and the updater is ready
        bool bootFirmware();

        /// @brief Re-runs the boot decision in place, the way a reset does:
        ///        the store and the updater are reconstructed and the flight
        ///        core starts from scratch, tuned values included, exactly
        ///        like the flash-less hardware this stands in for.
        void rebootFirmware();

        /// @brief Checks a slot's image against its own header, mirroring
        ///        drone_boot. One difference, and it is the whole point of a
        ///        process pretending to be a board: a slot that holds no
        ///        image header at all is this build itself, so it validates
        ///        instead of being marked bad.
        /// @param slot slot to validate
        /// @return true when the slot may be run
        [[nodiscard]] bool imageValidates(std::uint8_t slot) const;

        /// @brief Hands one received packet to the update session and
        ///        broadcasts whatever answer comes back, over the same
        ///        telemetry socket the tuning answers use.
        /// @param packet received bytes
        /// @param size received byte count
        /// @param nowUs monotonic time [us]
        /// @return true when the updater claimed the packet, whatever the
        ///         outcome: the rest of this composition must then ignore it
        bool serveOta(const std::uint8_t *packet, std::size_t size, std::uint64_t nowUs);

        /// @brief Parks the lockstep loop and serves the open update session,
        ///        symmetrically with the firmware: no sensor wait, no core
        ///        step, no actuator frame back to the plant, so the motors
        ///        are silent for the whole of it. Returns once the session
        ///        ends on a finish, an abort or a timeout.
        void runUpdateMode();

        /// @brief Refreshes the cached arming interlock from the boot
        ///        metadata. Reading it means scanning both metadata areas, so
        ///        it happens once per boot and after every packet the updater
        ///        consumed rather than once per frame.
        void refreshArmInterlock();

        /// @param data datagram bytes
        /// @param size datagram size
        /// @return true when this is the ground side's reboot command
        [[nodiscard]] static bool IsRebootCommand(const std::uint8_t *data, std::size_t size);

        /// @brief Routes one datagram drained from the command uplink. A
        ///        scenario packet is latched onto the motor sink, which
        ///        carries it to the plant on the next lockstep reply, and
        ///        its hash window is kept for the run it opens. Anything
        ///        else is not this composition's business.
        /// @param data datagram bytes
        /// @param size datagram size
        void forwardScenario(const std::uint8_t *data, std::size_t size);

        /// @brief Drains the command uplink: RC into the tracker, scenarios
        ///        to the plant, tuning answered. Runs on every loop wakeup,
        ///        frames or not - the command path needs no world, so tuning
        ///        a grounded drone works while the platform is not ready.
        /// @param nowUs instant handed to the RC fail-safe [us]
        void drainCommands(std::uint64_t nowUs);

        std::uint32_t m_maxFrames;     ///< frame budget for run()
        std::uint16_t m_simPort;       ///< sim link listen port
        std::uint16_t m_telemetryPort; ///< telemetry broadcast port
        std::uint16_t m_rcPort;        ///< command receiver listen port
        std::uint32_t m_sessionId;     ///< identity of this process start

        // Declaration order = construction order; dependencies are injected by
        // reference, so a service may only depend on those declared above it.
        mark4::ClockSim m_clock;
        mark4::UdpLink m_simLink;
        mark4::SensorSourceSim m_sensorSource;
        mark4::MotorSinkSim m_motorSink;
        mark4::TelemetrySenderSim m_telemetrySender;
        mark4::TelemetryPublisher m_telemetryPublisher{m_telemetrySender, StreamSource::DRONE_SIM};
        mark4::UdpLink m_rcLink;
        mark4::CommandReceiverSim m_commandReceiver{m_rcLink};
        mark4::RcTracker m_rcTracker{m_commandReceiver};
        mark4::TelemetrySenderSim m_announceSender;
        mark4::AnnouncePublisher m_announcePublisher{
            m_announceSender, StreamSource::DRONE_SIM, m_sessionId, m_telemetryPort, m_rcPort};
        std::array<char, LOG_PATH_SIZE> m_logFilePath; ///< one file per run, outlives m_logSink
        mark4::LogSinkFile m_logSink;
        mark4::FlightCore m_core;
        mark4::TuningService m_tuningService{m_core, m_telemetrySender};
        mark4::Blackbox m_blackbox;
        mark4::SimRunTracker m_runTracker{m_telemetrySender, StreamSource::DRONE_SIM};

        /// Emulated flash directory, declared before the store because the
        /// store keeps the pointer rather than a copy of the path.
        std::array<char, OTA_DIRECTORY_SIZE> m_otaDirectory{};

        /// The store and the updater are optional because a reboot command
        /// rebuilds both in place: which slot runs is a boot-time decision
        /// here, not a link-time one, so it cannot be a constructor argument
        /// settled once. std::optional keeps them value members all the same,
        /// with no allocation.
        std::optional<mark4::FirmwareStoreSim> m_firmwareStore;
        std::optional<mark4::OtaUpdater> m_otaUpdater;

        /// True while the running slot is on trial: arming is refused until
        /// the ground side confirms it (docs/ota-design.md section 3.2).
        bool m_armInhibited = false;

        /// Hash window asked for by the last scenario, applied to the run
        /// that scenario opens [us]; 0 means the tracker default.
        std::uint32_t m_pendingHashWindowUs = 0U;

        std::uint32_t m_lastSessionId = 0U; ///< simulator session of the last frame
    };
} // namespace mark4
