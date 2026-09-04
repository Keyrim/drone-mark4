#pragma once

/// @file
/// @brief drone_sim composition root.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "flight_core/flight_core.hpp"
#include "log/console_sink_posix.hpp"
#include "log/wire.hpp"
#include "ota/updater.hpp"
#include "platform_common/command_receiver_transport.hpp"
#include "platform_common/frame_telemetry.hpp"
#include "platform_common/rc_tracker.hpp"
#include "platform_common/status_publisher.hpp"
#include "platform_common/telemetry_service.hpp"
#include "platform_common/tuning_service.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/firmware_store_sim.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/plant_link.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/sim_run_tracker.hpp"
#include "platform_sim/truth_telemetry.hpp"
#include "protocol/envelope.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"

namespace mark4
{
    /// Composition root: owns every service and the flight core as value members.
    /// Member declaration order IS the construction/initialization order, and
    /// destruction is guaranteed to run in the exact reverse order - no manual
    /// teardown. Built by main(), passed by reference: no singleton.
    class DroneSimApp
    {
      public:
        /// Size of the buffer holding the emulated-flash directory. The
        /// store builds its own file paths inside it, so this only has to
        /// hold the directory itself.
        static constexpr std::size_t OTA_DIRECTORY_SIZE = 192U;

        /// Poll period of the parked update loop [us]. Short enough that the
        /// sender's chunk pacing is never the thing waiting, long enough that
        /// a transfer does not spin a core flat out.
        static constexpr std::uint32_t UPDATE_POLL_US = 500U;

        /// Fastest telemetry period this process serves [ms]: the frame
        /// period itself, since the plant paces the loop at 500 Hz and a
        /// loopback datagram costs nothing. There is nothing faster to ask
        /// for - a shorter period would only repeat a frame's values.
        static constexpr std::uint32_t MIN_TELEMETRY_PERIOD_MS = 2U;

        /// @param maxFrames number of frames to process before stopping,
        ///        0 = no limit (the run ends with the operator or the link)
        /// @param discoveryPort shared transport port of this deployment
        /// @param nodeId transport identity of this process, never 0; drawn
        ///        at random by main() unless a campaign pins it
        /// @param otaDirectory directory holding the emulated flash slots and
        ///        boot metadata; copied, so the caller keeps its buffer
        explicit DroneSimApp(std::uint32_t maxFrames,
                             std::uint16_t discoveryPort,
                             std::uint32_t nodeId,
                             const char *otaDirectory);

        /// @brief Initializes services in declaration order: opens the
        ///        transport, then runs the fake bootloader that picks the
        ///        firmware slot. The first failure is logged by the service
        ///        and returns false immediately.
        /// @return true when every service is ready
        bool init();

        /// @brief Runs the waitFrame -> step -> push -> record loop until the
        ///        requested number of frames is reached. Whether a plant
        ///        drives the frames is the platform's business: without one
        ///        the frames come without sensors and the core stays idle.
        /// @return number of steps executed
        std::uint32_t run();

        /// @return transport, for post-run reporting
        [[nodiscard]] const mark4::Transport &accessTransport() const
        {
            return m_transport;
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

        /// @return telemetry service, for post-run reporting
        [[nodiscard]] const mark4::TelemetryService &accessTelemetryService() const
        {
            return m_telemetryService;
        }

      private:
        /// @brief The fake bootloader: runs the slot decision shared with
        ///        drone_boot (ota/boot_policy.hpp) over the
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

        /// @brief Hands one received message to the update session and
        ///        broadcasts whatever answer comes back, over the same
        ///        telemetry route the tuning answers use.
        /// @param envelope decoded message
        /// @param nowUs monotonic time [us]
        /// @return true when the updater claimed the message, whatever the
        ///         outcome: the rest of this composition must then ignore it
        bool serveOta(const mark4_Envelope &envelope, std::uint64_t nowUs);

        /// @brief Parks the lockstep loop and serves the open update session,
        ///        symmetrically with the firmware: no sensor wait, no core
        ///        step, no actuator frame back to the plant, so the motors
        ///        are silent for the whole of it. Returns once the session
        ///        ends on a finish, an abort or a timeout.
        void runUpdateMode();

        /// @brief Refreshes the cached arming interlock from the boot
        ///        metadata. Reading it means scanning both metadata areas, so
        ///        it happens once per boot and after every message the updater
        ///        consumed rather than once per frame.
        void refreshArmInterlock();

        /// @brief Drains the command uplink: RC into the tracker, scenarios
        ///        to the plant, tuning answered, the updater served. Runs on
        ///        every frame, sensors or not - the command path needs no
        ///        world, so tuning a grounded drone works without a plant.
        /// @param nowUs instant handed to the RC fail-safe [us]
        void drainCommands(std::uint64_t nowUs);

        /// @brief Route of every log line and of the module table: a
        ///        transport broadcast, like everything this process emits.
        static bool SendLog(void *context, const std::uint8_t *data, std::size_t size);

        /// @brief Clock the log records are stamped with: simulated time.
        static std::uint64_t LogClock(void *context);

        /// @brief Broadcasts the module table (LogModules pages).
        void publishLogModules();

        std::uint32_t m_maxFrames; ///< frame budget for run()

        // Declaration order = construction order; dependencies are injected by
        // reference, so a service may only depend on those declared above it.
        mark4::ClockSim m_clock;
        mark4::UdpLink m_udpLink;
        mark4::Transport m_transport;
        mark4::ConsoleSinkPosix m_consoleSink;
        mark4::TransportSink m_transportSink{&DroneSimApp::SendLog, this};
        mark4::CommandReceiverTransport m_commandReceiver;
        /// The sim link: the plant's frames off the transport, sorted for
        /// the sensor source and the command receiver; the wait point of
        /// the flight loop sleeps on the link's sockets through it.
        mark4::PlantLink m_plantLink{m_transport, m_udpLink, m_clock, m_commandReceiver};
        mark4::SensorSourceSim m_sensorSource{m_plantLink, m_clock};
        mark4::MotorSinkSim m_motorSink{m_plantLink};
        mark4::StatusPublisher m_statusPublisher{m_transport};
        mark4::RcTracker m_rcTracker;
        /// Declared before the core so the ids of the platform measures come
        /// first in the frozen table: the order of construction IS the order
        /// of the ids (see components/telemetry/README.md).
        mark4::FrameTelemetry m_frameTelemetry;
        mark4::FlightCore m_core;
        mark4::TruthTelemetry m_truthTelemetry;
        mark4::TuningService m_tuningService{m_core, m_transport};
        mark4::SimRunTracker m_runTracker{m_transport};
        /// Last of the services: init() freezes the registry, so every
        /// object holding a measure must exist before it runs.
        mark4::TelemetryService m_telemetryService{m_transport, MIN_TELEMETRY_PERIOD_MS};

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

        /// The module table goes out once the first beacon did.
        bool m_logModulesPublished = false;
    };
} // namespace mark4
