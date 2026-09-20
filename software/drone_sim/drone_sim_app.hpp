#pragma once

/// @file
/// @brief drone_sim composition root.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "discovery/discovery.hpp"
#include "flight_core/flight_core.hpp"
#include "log/console_sink_posix.hpp"
#include "log/provider.hpp"
#include "messaging/messenger.hpp"
#include "ota/flight_gate.hpp"
#include "ota/provider.hpp"
#include "ota/updater.hpp"
#include "platform_common/frame_telemetry.hpp"
#include "platform_common/rc_tracker.hpp"
#include "platform_sim/clock_sim.hpp"
#include "platform_sim/firmware_store_sim.hpp"
#include "platform_sim/motor_sink_sim.hpp"
#include "platform_sim/plant_link.hpp"
#include "platform_sim/sensor_source_sim.hpp"
#include "platform_sim/sim_run_tracker.hpp"
#include "platform_sim/truth_telemetry.hpp"
#include "protocol/envelope.hpp"
#include "status/status_provider.hpp"
#include "telemetry/provider.hpp"
#include "transport/provider.hpp"
#include "transport/transport.hpp"
#include "transport/udp_link.hpp"
#include "tuning/provider.hpp"

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

        /// @return telemetry provider, for post-run reporting
        [[nodiscard]] const mark4::TelemetryProvider &accessTelemetryProvider() const
        {
            return m_telemetryProvider;
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

        /// The composition's own commands: what is neither a service's nor the plant's.
        class Commands final : public mark4::AbsMessageHandler
        {
          public:
            /// Body tags this handler consumes.
            static constexpr std::array<pb_size_t, 3> TAGS = {
                mark4_Envelope_rc_tag, mark4_Envelope_reboot_tag, mark4_Envelope_sim_scenario_tag};

            /// @param messenger messenger to attach to
            /// @param app composition the commands act on
            Commands(mark4::Messenger &messenger, DroneSimApp &app);

            bool onMessage(std::uint32_t src,
                           const mark4_Envelope &envelope,
                           std::uint64_t nowUs) override;

          private:
            DroneSimApp &m_app; ///< the composition, not owned
        };

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

        /// @brief What this process answers to an IdentityRequest.
        /// @return the announce: kind DRONE_SIM, mcu SIM, this build's wire
        ///         hash, and no build identity
        static mark4_Announce Identity();

        /// @brief Clock the log records are stamped with: simulated time.
        static std::uint64_t LogClock(void *context);

        std::uint32_t m_maxFrames; ///< frame budget for run()

        // Declaration order = construction order; dependencies are injected by
        // reference, so a service may only depend on those declared above it.
        mark4::ClockSim m_clock;
        mark4::UdpLink m_udpLink;
        mark4::Transport m_transport;
        /// The requests waiting for their acknowledgement, owned here and
        /// handed to the messenger as a span.
        std::array<mark4::PendingRequest, mark4::Messenger::BOARD_PENDING_REQUESTS>
            m_pendingRequests{};
        /// Every message addressed to this node goes through it to the one
        /// handler of its tag; every handler below is declared after it.
        mark4::Messenger m_messenger{m_transport, m_pendingRequests};
        Commands m_commands{m_messenger, *this};
        /// Who this process is, to whoever asks.
        mark4::Discovery m_discovery{m_messenger, Identity()};
        mark4::ConsoleSinkPosix m_consoleSink;
        /// This node's log on the wire: the lines to whoever subscribed, the
        /// module table one page per request, the levels.
        mark4::LogProvider m_logProvider{m_messenger};
        /// What this node's transport and messenger count, to whoever
        /// subscribed: one report a second, its peer table by pages.
        mark4::TransportProvider m_transportProvider{m_messenger, m_transport};
        /// The sim link: the handler of the plant's sensor messages, and the
        /// one caller of the messenger's poll; the wait point of the flight
        /// loop sleeps on the link's sockets through it.
        mark4::PlantLink m_plantLink{m_messenger, m_transport, m_udpLink, m_clock};
        mark4::SensorSourceSim m_sensorSource{m_plantLink, m_clock};
        mark4::MotorSinkSim m_motorSink{m_plantLink};
        mark4::StatusProvider m_statusProvider{m_messenger};
        mark4::RcTracker m_rcTracker;
        /// Declared before the core so the ids of the platform measures come
        /// first in the frozen table: the order of construction IS the order
        /// of the ids (see components/telemetry/README.md).
        mark4::FrameTelemetry m_frameTelemetry;
        mark4::FlightCore m_core;
        /// What the updater asks this node about itself before a session.
        mark4::FlightOtaGate m_otaGate{m_core};
        mark4::TruthTelemetry m_truthTelemetry;
        mark4::TuningProvider m_tuningProvider{m_messenger, m_core};
        mark4::SimRunTracker m_runTracker;
        /// Last of the providers: init() freezes the registry, so every
        /// object holding a measure must exist before it runs.
        mark4::TelemetryProvider m_telemetryProvider{m_messenger, MIN_TELEMETRY_PERIOD_MS};

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
        /// The updater's handler, rebuilt with it: it holds a reference to
        /// the updater it serves.
        std::optional<mark4::OtaProvider> m_otaProvider;

        /// True while the running slot is on trial: arming is refused until
        /// the ground side confirms it (docs/ota-design.md section 3.2).
        bool m_armInhibited = false;

        /// OtaProvider::consumed() at the last interlock refresh.
        std::uint32_t m_otaConsumedSeen = 0U;

        /// Timestamp of the last frame waitFrame() returned: the flight time
        /// base an RC packet is stamped with. The messenger is polled inside
        /// the wait, before the frame about to arrive is known, so the
        /// previous frame is the freshest instant of that base; it is at
        /// most one period behind, which the RC fail-safe timeout does not
        /// see.
        std::uint64_t m_lastFrameUs = 0U;

        /// A Reboot arrived, acted on after the wait.
        bool m_rebootRequested = false;

        /// Hash window asked for by the last scenario, applied to the run
        /// that scenario opens [us]; 0 means the tracker default.
        std::uint32_t m_pendingHashWindowUs = 0U;
    };
} // namespace mark4
