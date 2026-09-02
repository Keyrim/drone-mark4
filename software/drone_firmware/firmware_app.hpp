#pragma once

/// @file
/// @brief firmware composition root.

#include <cstddef>
#include <cstdint>

#include "flight_core/flight_core.hpp"
#include "flight_core/types.hpp"
#include "log/wire.hpp"
#include "platform_common/command_receiver_transport.hpp"
#include "platform_common/ota_updater.hpp"
#include "platform_common/rc_tracker.hpp"
#include "platform_common/status_publisher.hpp"
#include "platform_common/tuning_service.hpp"
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
    /// relay. Everything it emits (telemetry, tuning and updater answers,
    /// log lines, its Announce beacon) is a broadcast, like drone_sim: the
    /// relay puts it on the LAN and nobody has to know who asked.
    /// Commands reach it as unicasts to its node id, or as broadcasts.
    class FirmwareApp
    {
      public:
        /// Frames between two status lines (app/status, DEBUG): one per second.
        static constexpr std::uint32_t FRAMES_PER_STATUS = SensorSourceStm32::FRAME_RATE_HZ;

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
        /// @brief Hands one received message to the update session and sends
        ///        whatever answer comes back.
        /// @param envelope decoded message
        /// @param nowUs monotonic time [us]
        /// @return true when the updater claimed the message, whatever the
        ///         outcome: the rest of this composition must then ignore it
        bool serveOta(const mark4_Envelope &envelope, std::uint64_t nowUs);

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

        /// @brief Registers the Announce naming this board (kind, chip and
        ///        the identity stamped in the running slot's image header)
        ///        as the transport beacon.
        /// @return false when it does not fit a beacon
        bool setAnnounceBeacon();

        /// @brief Pumps the transport: frames in, beacon and expiry out.
        ///        Every payload delivered lands in the command receiver.
        /// @param nowUs current instant [us]
        void pollTransport(std::uint64_t nowUs);

        /// @brief Transport delivery callback: queues one payload.
        static void OnPayload(void *context,
                              std::uint32_t src,
                              const std::uint8_t *payload,
                              std::size_t size);

        /// @brief Drains the command receiver into the services: RC to the
        ///        tracker, updater messages served, tuning answered.
        /// @param nowUs instant handed to the RC fail-safe and the updater [us]
        /// @return true when a reboot command was drained
        bool drainCommands(std::uint64_t nowUs);

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
        mark4::RttSink m_rttSink;
        mark4::TransportSink m_transportSink{&FirmwareApp::SendLog, this};
        mark4::I2cBus m_bus;
        mark4::Mpu6050 m_imu{m_bus};
        mark4::Bmp581 m_baro{m_bus};
        mark4::SensorSourceStm32 m_sensorSource{m_imu, m_baro, m_clock};
        mark4::MotorSinkDshot m_motorSink;
        mark4::StatusPublisher m_statusPublisher{m_transport};
        mark4::CommandReceiverTransport m_commandReceiver;
        mark4::RcTracker m_rcTracker;
        mark4::FlightCore m_core;
        mark4::TuningService m_tuningService{m_core, m_transport};
        /// The slot this image was linked for is a compile-time fact
        /// (ota_slots.hpp, one -DDRONE_OTA_SLOT_ID per variant); the store
        /// refuses to erase or program it, whatever arrives on the wire.
        mark4::FirmwareStoreStm32 m_firmwareStore{mark4::OTA_RUNNING_SLOT};
        mark4::OtaUpdater m_otaUpdater{m_firmwareStore};

        /// The beacon as registered: the identity the boot line reports.
        mark4_Announce m_announce = mark4_Announce_init_zero;

        /// True while the running slot is on trial: arming is refused until
        /// the image confirms itself (docs/ota-design.md section 3.2).
        bool m_armInhibited = false;

        /// The module table goes out once the first beacon did.
        bool m_logModulesPublished = false;
    };
} // namespace mark4
