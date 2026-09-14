#pragma once

/// @file
/// @brief firmware composition root.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "discovery/discovery.hpp"
#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "log/wire.hpp"
#include "messaging/messenger.hpp"
#include "ota/updater.hpp"
#include "platform_common/frame_telemetry.hpp"
#include "platform_common/rc_tracker.hpp"
#include "platform_common/status_publisher.hpp"
#include "platform_stm32/bmp581.hpp"
#include "platform_stm32/board.hpp"
#include "platform_stm32/clock_stm32.hpp"
#include "platform_stm32/firmware_store_stm32.hpp"
#include "platform_stm32/i2c_bus.hpp"
#include "platform_stm32/motor_sink_dshot.hpp"
#include "platform_stm32/mpu6050.hpp"
#include "platform_stm32/ota_slots.hpp"
#include "platform_stm32/rtt_sink.hpp"
#include "platform_stm32/sensor_source_stm32.hpp"
#include "platform_stm32/uart1_stream.hpp"
#include "protocol/envelope.hpp"
#include "services/flight_ota_gate.hpp"
#include "services/ota_service.hpp"
#include "services/telemetry_service.hpp"
#include "services/tuning_service.hpp"
#include "telemetry/registry.hpp"
#include "transport/transport.hpp"
#include "transport/uart_link.hpp"

namespace mark4
{
    /// Composition root: owns every service and the flight core as value
    /// members. Member declaration order IS the construction order, and a
    /// service may only depend on those declared above it. Built by
    /// main(), passed by reference: no singleton.
    ///
    /// The board is one transport node with one link, USART1 to the ESP32
    /// relay. What it reports unasked (status, log lines) is a broadcast
    /// the relay puts on the LAN; what it answers (telemetry, tuning and
    /// updater replies) goes to the node that asked, through the messenger.
    /// Commands reach it as unicasts to its node id, or as broadcasts.
    class FirmwareApp
    {
      public:
        /// Frames between two status lines (app/status, DEBUG): one per second.
        static constexpr std::uint32_t FRAMES_PER_STATUS = SensorSourceStm32::FRAME_RATE_HZ;

        /// Fastest telemetry period this board serves [ms]. The line is the
        /// limit, not the loop: at 921600 baud with the serial framing it
        /// carries about 92 kB/s, and 64 enabled measures are two messages
        /// of roughly 300 bytes each, so 100 Hz is about 60 kB/s. That
        /// leaves room for the status stream, the log lines and the tuning
        /// answers sharing the same UART; asking for 1 ms would not.
        static constexpr std::uint32_t MIN_TELEMETRY_PERIOD_MS = 10U;

        FirmwareApp() = default;

        /// @brief Initializes the board (clock tree, RTT console, LEDs)
        ///        then the services in declaration order. The first
        ///        failure is logged (RTT and the transport) and returns
        ///        false.
        /// @return true when the loop is ready to run
        bool init();

        /// @brief Runs the waitFrame -> step -> push loop forever, with a
        ///        heartbeat LED and a one-line status over RTT every
        ///        second. An accepted update session parks that loop until
        ///        the session ends (see docs/ota-design.md section 3.2).
        [[noreturn]] void run();

      private:
        /// The composition's own commands: what is not a service's.
        class Commands final : public mark4::AbsMessageHandler
        {
          public:
            /// Body tags this handler consumes.
            static constexpr std::array<pb_size_t, 3> TAGS = {
                mark4_Envelope_rc_tag, mark4_Envelope_reboot_tag, mark4_Envelope_log_control_tag};

            /// @param messenger messenger to attach to
            /// @param app composition the commands act on
            Commands(mark4::Messenger &messenger, FirmwareApp &app);

            bool onMessage(std::uint32_t src,
                           const mark4_Envelope &envelope,
                           std::uint64_t nowUs) override;

          private:
            FirmwareApp &m_app; ///< the composition, not owned
        };

        /// @brief Parks the flight loop and serves the open update session:
        ///        no sensor read, no core step, no motor output at all, so
        ///        the ESCs observe silence and disarm. Returns once the
        ///        session ends on a finish, an abort or a timeout.
        void runUpdateMode();

        /// @brief Refreshes the cached arming interlock from the boot
        ///        metadata. Called once at init and after every message the
        ///        updater consumed, because reading the metadata means
        ///        scanning both flash sectors: far too expensive per frame,
        ///        and nothing else can move the running slot's state.
        void refreshArmInterlock();

        /// @brief Polls the messenger: frames in, keepalive and expiry out,
        ///        every message delivered dispatched to its handler.
        /// @param nowUs current instant [us]
        void pollTransport(std::uint64_t nowUs);

        /// @brief Route of every log line and of the module table: a
        ///        transport broadcast, like everything this board emits.
        static bool SendLog(void *context, const std::uint8_t *data, std::size_t size);

        /// @brief Clock the log records are stamped with.
        static std::uint64_t LogClock(void *context);

        /// @brief Broadcasts the module table (LogModules pages).
        void publishLogModules();

        // Declaration order = construction order; dependencies are
        // injected by reference, so a service may only depend on those
        // declared above it. The link comes first so that an init failure
        // further down is reported on it.
        mark4::ClockStm32 m_clock;
        mark4::Uart1Stream m_uartStream;
        mark4::UartLink m_uartLink{m_uartStream};
        mark4::Transport m_transport{boardNodeId()};
        /// Every message addressed to this node goes through it to the one
        /// handler of its tag; every handler below is declared after it.
        mark4::Messenger m_messenger{m_transport};
        Commands m_commands{m_messenger, *this};
        /// Who this board is, to whoever asks. Optional because the answer
        /// carries the identity stamped in the running image, read from the
        /// flash at init and not knowable before it.
        std::optional<mark4::Discovery>
            m_discovery; ///< who this board is, once its image identity is read
        mark4::RttSink m_rttSink;
        mark4::TransportSink m_transportSink{&FirmwareApp::SendLog, this};
        mark4::I2cBus m_bus;
        mark4::Mpu6050 m_imu{m_bus};
        mark4::Bmp581 m_baro{m_bus};
        mark4::SensorSourceStm32 m_sensorSource{m_imu, m_baro, m_clock};
        mark4::MotorSinkDshot m_motorSink;
        mark4::StatusPublisher m_statusPublisher{m_transport};
        mark4::RcTracker m_rcTracker;
        /// Declared before the core so the ids of the platform measures come
        /// first in the frozen table: the order of construction IS the order
        /// of the ids (see components/telemetry/README.md).
        mark4::FrameTelemetry m_frameTelemetry;
        mark4::FlightCore m_core;
        mark4::TuningService m_tuningService{m_messenger, m_core};
        /// The slot this image was linked for is a compile-time fact
        /// (ota_slots.hpp, one -DDRONE_OTA_SLOT_ID per variant); the store
        /// refuses to erase or program it, whatever arrives on the wire.
        mark4::FirmwareStoreStm32 m_firmwareStore{mark4::OTA_RUNNING_SLOT};
        mark4::OtaUpdater m_otaUpdater{m_firmwareStore};
        /// What the updater asks this node about itself before a session.
        mark4::FlightOtaGate m_otaGate{m_core};
        mark4::OtaService m_otaService{m_messenger, m_otaUpdater, m_otaGate};
        /// Last of the services: init() freezes the registry, so every
        /// object holding a measure must exist before it runs.
        mark4::TelemetryService m_telemetryService{m_messenger, MIN_TELEMETRY_PERIOD_MS};

        /// Wall time one waitFrame -> step -> push cycle took, refreshed
        /// every frame [us]: the one number that says whether the loop still
        /// fits its 2 ms budget.
        float m_stepDurationUs = 0.0f;
        mark4::TelemetryEntry m_stepDurationEntry{
            "loop/step_duration", mark4::TelemetryUnit::US, m_stepDurationUs};

        /// True while the running slot is on trial: arming is refused until
        /// the image confirms itself (docs/ota-design.md section 3.2).
        bool m_armInhibited = false;

        /// OtaService::consumed() at the last interlock refresh.
        std::uint32_t m_otaConsumedSeen = 0U;

        /// A Reboot arrived, acted on after the poll.
        bool m_rebootRequested = false;

        /// The module table goes out once the first keepalive did.
        bool m_logModulesPublished = false;
    };
} // namespace mark4
