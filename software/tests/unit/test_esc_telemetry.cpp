#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "platform_stm32/esc_telemetry.hpp"

using namespace mark4;

namespace
{
    /// @brief Builds a KISS frame from its fields, CRC included.
    std::array<std::uint8_t, KISS_FRAME_SIZE> kissFrame(std::uint8_t temperatureC,
                                                        std::uint16_t centiVolts,
                                                        std::uint16_t centiAmps,
                                                        std::uint16_t mah,
                                                        std::uint16_t erpmHundreds)
    {
        std::array<std::uint8_t, KISS_FRAME_SIZE> frame{};
        frame[0] = temperatureC;
        frame[1] = static_cast<std::uint8_t>(centiVolts >> 8U);
        frame[2] = static_cast<std::uint8_t>(centiVolts & 0xFFU);
        frame[3] = static_cast<std::uint8_t>(centiAmps >> 8U);
        frame[4] = static_cast<std::uint8_t>(centiAmps & 0xFFU);
        frame[5] = static_cast<std::uint8_t>(mah >> 8U);
        frame[6] = static_cast<std::uint8_t>(mah & 0xFFU);
        frame[7] = static_cast<std::uint8_t>(erpmHundreds >> 8U);
        frame[8] = static_cast<std::uint8_t>(erpmHundreds & 0xFFU);
        frame[9] = kissCrc8(frame.data(), KISS_FRAME_SIZE - 1U);
        return frame;
    }

    /// @brief Feeds a whole frame byte by byte at one instant.
    void feedFrame(EscTelemetry &esc,
                   const std::array<std::uint8_t, KISS_FRAME_SIZE> &frame,
                   std::uint64_t nowUs)
    {
        for (const std::uint8_t byte : frame)
        {
            esc.feed(byte, nowUs);
        }
    }

    constexpr std::uint64_t FRAME_US = 2000U;
} // namespace

TEST_CASE("kiss crc8 matches the reference vectors")
{
    // CRC-8 (poly 0x07, init 0): the check value of "123456789" is 0xF4.
    const std::array<std::uint8_t, 9> check = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    REQUIRE(kissCrc8(check.data(), check.size()) == 0xF4U);
    const std::array<std::uint8_t, 1> zero = {0x00U};
    REQUIRE(kissCrc8(zero.data(), zero.size()) == 0x00U);
    const std::array<std::uint8_t, 1> one = {0x01U};
    REQUIRE(kissCrc8(one.data(), one.size()) == 0x07U);
}

TEST_CASE("kiss frame decodes big-endian fields into their units")
{
    // 37 degC, 16.42 V, 12.34 A, 1500 mAh, 24500 eRPM (245 x 100).
    const auto frame = kissFrame(37U, 1642U, 1234U, 1500U, 245U);
    EscTelemetrySample sample;
    REQUIRE(decodeKissFrame(frame.data(), sample));
    REQUIRE(std::fabs(sample.temperatureC - 37.0f) < 1e-6f);
    REQUIRE(std::fabs(sample.voltageV - 16.42f) < 1e-4f);
    REQUIRE(std::fabs(sample.currentA - 12.34f) < 1e-4f);
    REQUIRE(std::fabs(sample.consumptionMah - 1500.0f) < 1e-6f);
    REQUIRE(std::fabs(sample.erpm - 24500.0f) < 1e-6f);

    // A corrupted byte fails the CRC and leaves the sample untouched.
    auto corrupted = frame;
    corrupted[2] ^= 0x10U;
    EscTelemetrySample untouched;
    REQUIRE(!(decodeKissFrame(corrupted.data(), untouched)));
    REQUIRE(untouched.voltageV == 0.0f);
}

TEST_CASE("esc telemetry asks one esc at a time, round-robin")
{
    EscTelemetry esc;
    std::size_t motor = ESC_COUNT;
    std::uint64_t nowUs = 0U;

    for (std::size_t turn = 0U; turn < 2U * ESC_COUNT; ++turn)
    {
        // One request per frame, then the ESC answers before the next one.
        REQUIRE(esc.nextRequest(nowUs, motor));
        REQUIRE(motor == turn % ESC_COUNT);
        // While the request is outstanding no other one goes out.
        std::size_t other = ESC_COUNT;
        REQUIRE(!(esc.nextRequest(nowUs + 1U, other)));
        feedFrame(esc, kissFrame(static_cast<std::uint8_t>(20U + motor), 1600U, 0U, 0U, 0U), nowUs);
        nowUs += FRAME_US;
    }

    REQUIRE(esc.frames() == 2U * ESC_COUNT);
    REQUIRE(esc.crcErrors() == 0U);
    REQUIRE(esc.timeouts() == 0U);
    for (std::size_t index = 0U; index < ESC_COUNT; ++index)
    {
        REQUIRE(std::fabs(esc.sample(index).temperatureC - static_cast<float>(20.0 + index)) <
                1e-6f);
        REQUIRE(esc.online(index, nowUs));
    }
}

TEST_CASE("esc telemetry skips an esc that does not answer")
{
    EscTelemetry esc;
    std::size_t motor = ESC_COUNT;
    std::uint64_t nowUs = 0U;

    REQUIRE(esc.nextRequest(nowUs, motor));
    REQUIRE(motor == 0U);
    // Silence: the request stays outstanding for the whole timeout...
    nowUs += EscTelemetry::REQUEST_TIMEOUT_US - 1U;
    REQUIRE(!(esc.nextRequest(nowUs, motor)));
    // ...then the turn passes to the next ESC and the miss is counted.
    nowUs += 1U;
    REQUIRE(esc.nextRequest(nowUs, motor));
    REQUIRE(motor == 1U);
    REQUIRE(esc.timeouts() == 1U);
    REQUIRE(!(esc.online(0U, nowUs)));

    // A bad frame is rejected, but the turn still moves on.
    auto bad = kissFrame(30U, 1600U, 0U, 0U, 0U);
    bad[9] ^= 0xFFU;
    feedFrame(esc, bad, nowUs);
    REQUIRE(esc.crcErrors() == 1U);
    REQUIRE(esc.frames() == 0U);
    REQUIRE(esc.nextRequest(nowUs, motor));
    REQUIRE(motor == 2U);
}

TEST_CASE("esc telemetry drops bytes outside a request and ages out a silent esc")
{
    EscTelemetry esc;
    std::size_t motor = ESC_COUNT;
    std::uint64_t nowUs = 0U;

    // Nothing was asked yet: whatever comes in is stray.
    esc.feed(0x55U, nowUs);
    REQUIRE(esc.strayBytes() == 1U);
    REQUIRE(esc.frames() == 0U);

    REQUIRE(esc.nextRequest(nowUs, motor));
    feedFrame(esc, kissFrame(25U, 1600U, 0U, 0U, 0U), nowUs);
    REQUIRE(esc.online(0U, nowUs));
    // Trailing garbage after a complete frame lands on nobody.
    esc.feed(0xAAU, nowUs);
    REQUIRE(esc.strayBytes() == 2U);

    // The ESC that reported goes silent after SILENCE_US without a frame.
    REQUIRE(esc.online(0U, nowUs + EscTelemetry::SILENCE_US - 1U));
    REQUIRE(!(esc.online(0U, nowUs + EscTelemetry::SILENCE_US)));
}
