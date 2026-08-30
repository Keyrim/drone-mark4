/// @file
/// @brief The tuning adapter: which messages it claims and what it answers
///        with.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "flight_core/attitude_estimator.hpp"
#include "flight_core/flight_core.hpp"
#include "flight_core/rate_controller.hpp"
#include "flight_core/tuning_table.hpp"
#include "flight_core/types.hpp"
#include "platform/telemetry_sender.hpp"
#include "platform_common/tuning_service.hpp"
#include "protocol/envelope.hpp"

namespace
{
    constexpr std::uint64_t STEP_US = 2000U; // 500 Hz stream
    constexpr float HELPER_BARO_PA = 101325.0f;

    /// Telemetry sender keeping every datagram handed to it, so a test can
    /// check the exact bytes that went out. Allocates freely: this is a test.
    class FakeTelemetrySender final : public mark4::AbsTelemetrySender
    {
      public:
        void send(const std::uint8_t *data, std::size_t size) override
        {
            m_sent.emplace_back(data, data + size);
        }

        /// @return datagrams captured since construction
        [[nodiscard]] const std::vector<std::vector<std::uint8_t>> &sent() const
        {
            return m_sent;
        }

        /// @brief Forgets everything captured so far.
        void clear()
        {
            m_sent.clear();
        }

      private:
        std::vector<std::vector<std::uint8_t>> m_sent; ///< captured datagrams
    };

    /// @param id parameter id
    /// @param value requested value
    /// @return one TuningSet
    mark4_Envelope makeSet(std::uint32_t id, float value)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_tuning_set_tag;
        envelope.body.tuning_set.id = id;
        envelope.body.tuning_set.value = value;
        return envelope;
    }

    /// @param id parameter id
    /// @return one TuningGet
    mark4_Envelope makeGet(std::uint32_t id)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_tuning_get_tag;
        envelope.body.tuning_get.id = id;
        return envelope;
    }

    /// @param startIndex first table index to describe
    /// @return one TuningList
    mark4_Envelope makeList(std::uint32_t startIndex)
    {
        mark4_Envelope envelope = mark4_Envelope_init_zero;
        envelope.which_body = mark4_Envelope_tuning_list_tag;
        envelope.body.tuning_list.start_index = startIndex;
        return envelope;
    }

    /// @param datagram bytes to decode
    /// @return the envelope the datagram carries
    mark4_Envelope decode(const std::vector<std::uint8_t> &datagram)
    {
        mark4_Envelope envelope;
        REQUIRE(mark4::decodeEnvelope(datagram.data(), datagram.size(), envelope));
        return envelope;
    }

    /// @param datagram bytes to decode
    /// @return the ack the datagram carries
    mark4_TuningAck decodeAck(const std::vector<std::uint8_t> &datagram)
    {
        const mark4_Envelope envelope = decode(datagram);
        REQUIRE(envelope.which_body == mark4_Envelope_tuning_ack_tag);
        return envelope.body.tuning_ack;
    }

    /// @param datagram bytes to decode
    /// @return the parameter description the datagram carries
    mark4_TuningInfo decodeInfo(const std::vector<std::uint8_t> &datagram)
    {
        const mark4_Envelope envelope = decode(datagram);
        REQUIRE(envelope.which_body == mark4_Envelope_tuning_info_tag);
        return envelope.body.tuning_info;
    }

    /// @brief Drives a core to ARMED: settled on the ground, altitude-auto
    ///        selected, stick centered, arm switch on.
    /// @param core core to drive
    void driveToArmed(mark4::FlightCore &core)
    {
        mark4::ActuatorFrame actuators;
        mark4::SensorFrame frame;
        frame.rc.killSwitch = false;
        frame.rc.mode = mark4::PilotMode::ALTITUDE_AUTO;
        frame.rc.throttle = 0.5f;
        frame.accelMps2 = {0.0f, 0.0f, mark4::GRAVITY_MPS2};
        frame.baroPa = HELPER_BARO_PA;
        frame.imuValid = true;
        frame.baroValid = true;
        for (std::uint32_t i = 0U; i < 200U; ++i)
        {
            frame.timestampUs = static_cast<std::uint64_t>(i + 1U) * STEP_US;
            frame.rc.armSwitch = i >= 100U;
            core.step(frame, actuators);
        }
        REQUIRE(core.flightPhase() == mark4::FlightPhase::ARMED);
    }
} // namespace

TEST_CASE("a tuning set is applied and acknowledged with the value in effect")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    REQUIRE(service.handle(makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 0.7f)));
    REQUIRE(service.requestCount() == 1U);
    REQUIRE(sender.sent().size() == 1U);

    const mark4_TuningAck ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.id == mark4::TUNING_ID_HOVER_COLLECTIVE);
    REQUIRE(ack.status == mark4_TuningStatus_OK);
    REQUIRE(ack.value == 0.7f);

    // The value the ack carries is the one the core is really flying.
    float live = 0.0f;
    REQUIRE(core.getParam(mark4::TUNING_ID_HOVER_COLLECTIVE, live) == mark4::TuningStatus::OK);
    REQUIRE(live == 0.7f);
}

TEST_CASE("an out-of-bounds tuning set is refused and the live value survives")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    float before = 0.0f;
    REQUIRE(core.getParam(mark4::TUNING_ID_HOVER_COLLECTIVE, before) == mark4::TuningStatus::OK);

    REQUIRE(service.handle(makeSet(mark4::TUNING_ID_HOVER_COLLECTIVE, 42.0f)));
    const mark4_TuningAck ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.status == mark4_TuningStatus_OUT_OF_BOUNDS);
    // The refusal answers with what is still flying, not with what was asked.
    REQUIRE(ack.value == before);
}

TEST_CASE("an unknown parameter id is acknowledged as unknown")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    REQUIRE(service.handle(makeSet(9999U, 1.0f)));
    const mark4_TuningAck ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.id == 9999U);
    REQUIRE(ack.status == mark4_TuningStatus_UNKNOWN_ID);
    REQUIRE(ack.value == 0.0f);
}

TEST_CASE("a parameter locked while armed is refused with its own status")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);
    driveToArmed(core);

    REQUIRE(service.handle(makeSet(mark4::TUNING_ID_AHRS_KP, 3.0f)));
    const mark4_TuningAck ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.status == mark4_TuningStatus_LOCKED_WHILE_ARMED);
    REQUIRE(ack.value == mark4::AttitudeEstimator::DEFAULT_KP);

    // A gain a pilot does retune between throws goes through.
    sender.clear();
    REQUIRE(service.handle(makeSet(mark4::TUNING_ID_RATE_KP_ROLL_PITCH, 0.05f)));
    REQUIRE(decodeAck(sender.sent()[0]).status == mark4_TuningStatus_OK);
}

TEST_CASE("a tuning get reads a value back without changing it")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);
    REQUIRE(core.setParam(mark4::TUNING_ID_VERTICAL_KP, 0.42f) == mark4::TuningStatus::OK);

    REQUIRE(service.handle(makeGet(mark4::TUNING_ID_VERTICAL_KP)));
    const mark4_TuningAck ack = decodeAck(sender.sent()[0]);
    REQUIRE(ack.id == mark4::TUNING_ID_VERTICAL_KP);
    REQUIRE(ack.status == mark4_TuningStatus_OK);
    REQUIRE(ack.value == 0.42f);

    REQUIRE(service.handle(makeGet(1234U)));
    REQUIRE(decodeAck(sender.sent()[1]).status == mark4_TuningStatus_UNKNOWN_ID);
}

TEST_CASE("a tuning list unrolls one description per pump, in table order")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    REQUIRE(service.handle(makeList(0U)));
    // The request itself emits nothing: the answer is paced by pump().
    REQUIRE(sender.sent().empty());

    // One extra pump past the end must add nothing.
    for (std::size_t i = 0U; i <= mark4::FlightCore::ParamCount(); ++i)
    {
        service.pump();
    }
    REQUIRE(sender.sent().size() == mark4::FlightCore::ParamCount());

    for (std::size_t i = 0U; i < mark4::FlightCore::ParamCount(); ++i)
    {
        const mark4_TuningInfo info = decodeInfo(sender.sent()[i]);
        INFO("entry " << i);
        REQUIRE(info.index == i);
        REQUIRE(info.count == mark4::FlightCore::ParamCount());

        // Everything the description carries must match the registry entry
        // it describes: the ground side reads this instead of a table of its
        // own, so a mismatch here is a silently wrong ground station.
        const mark4::TuningParam *param = core.paramInfo(i);
        REQUIRE(param != nullptr);
        REQUIRE(info.id == param->id);
        REQUIRE(info.value == param->value);
        REQUIRE(info.min_value == param->minValue);
        REQUIRE(info.max_value == param->maxValue);
        REQUIRE(info.armed_change == param->armedChange);

        const std::string name(info.name);
        REQUIRE(!name.empty());
        REQUIRE(name.size() <= mark4::TuningParam::NAME_SIZE);
    }

    // Pumping again with nothing pending stays silent.
    service.pump();
    REQUIRE(sender.sent().size() == mark4::FlightCore::ParamCount());
}

TEST_CASE("a tuning list starting past the end answers nothing")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    REQUIRE(service.handle(makeList(static_cast<std::uint32_t>(mark4::FlightCore::ParamCount()))));
    for (std::size_t i = 0U; i < 5U; ++i)
    {
        service.pump();
    }
    REQUIRE(sender.sent().empty());
}

TEST_CASE("a new tuning list restarts the walk mid-stream")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    REQUIRE(service.handle(makeList(0U)));
    service.pump();
    service.pump();
    REQUIRE(sender.sent().size() == 2U);
    REQUIRE(decodeInfo(sender.sent()[1]).index == 1U);

    // A second request repositions the cursor: this is how a ground station
    // asks again for the entries a lost frame took with it.
    sender.clear();
    REQUIRE(service.handle(makeList(3U)));
    service.pump();
    REQUIRE(sender.sent().size() == 1U);
    REQUIRE(decodeInfo(sender.sent()[0]).index == 3U);
}

TEST_CASE("a message that is not a tuning request is left to its owner")
{
    mark4::FlightCore core;
    FakeTelemetrySender sender;
    mark4::TuningService service(core, sender);

    mark4_Envelope reboot = mark4_Envelope_init_zero;
    reboot.which_body = mark4_Envelope_reboot_tag;
    REQUIRE(!service.handle(reboot));
    REQUIRE(!service.handle(mark4_Envelope_init_zero));

    REQUIRE(service.requestCount() == 0U);
    REQUIRE(sender.sent().empty());
}
