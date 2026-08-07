#include <array>
#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "protocol/serial_framing.hpp"

namespace
{
    /// @brief Runs a byte buffer through a parser and collects the payloads.
    /// @param parser decoder under test
    /// @param data stream bytes
    /// @param size stream size
    /// @param[out] payloads sizes of the payloads decoded, in order
    /// @return number of frames decoded
    std::size_t feedAll(mark4::SerialFrameParser &parser,
                        const std::uint8_t *data,
                        std::size_t size,
                        std::array<std::size_t, 8> &payloads)
    {
        std::size_t frames = 0U;
        for (std::size_t index = 0U; index < size; ++index)
        {
            const std::size_t decoded = parser.feed(data[index]);
            if (decoded != 0U && frames < payloads.size())
            {
                payloads[frames] = decoded;
                ++frames;
            }
        }
        return frames;
    }
} // namespace

TEST_CASE("serial framing round-trips a payload")
{
    const std::array<std::uint8_t, 5> payload = {1U, 2U, 3U, 4U, 250U};
    std::array<std::uint8_t, payload.size() + mark4::SERIAL_FRAME_OVERHEAD> frame{};
    REQUIRE(mark4::encodeSerialFrame(payload.data(), payload.size(), frame.data()) == frame.size());

    mark4::SerialFrameParser parser;
    std::array<std::size_t, 8> sizes{};
    REQUIRE(feedAll(parser, frame.data(), frame.size(), sizes) == 1U);
    REQUIRE(sizes[0] == payload.size());
    REQUIRE(std::memcmp(parser.payload(), payload.data(), payload.size()) == 0);
}

TEST_CASE("serial framing resynchronizes after garbage between frames")
{
    const std::array<std::uint8_t, 3> payload = {0xAAU, 0xA5U, 0x5AU};
    std::array<std::uint8_t, 32> stream{};
    std::size_t used = 0U;
    stream[used++] = 0x42U; // leading noise
    stream[used++] = mark4::SERIAL_SYNC0;
    stream[used++] = 0x00U; // broken sync pair
    used += mark4::encodeSerialFrame(payload.data(), payload.size(), &stream[used]);
    stream[used++] = mark4::SERIAL_SYNC0; // trailing noise that looks like a start
    used += mark4::encodeSerialFrame(payload.data(), payload.size(), &stream[used]);

    mark4::SerialFrameParser parser;
    std::array<std::size_t, 8> sizes{};
    REQUIRE(feedAll(parser, stream.data(), used, sizes) >= 1U);
    REQUIRE(sizes[0] == payload.size());
    REQUIRE(std::memcmp(parser.payload(), payload.data(), payload.size()) == 0);
}

TEST_CASE("serial framing rejects a corrupted checksum")
{
    const std::array<std::uint8_t, 4> payload = {9U, 8U, 7U, 6U};
    std::array<std::uint8_t, payload.size() + mark4::SERIAL_FRAME_OVERHEAD> frame{};
    REQUIRE(mark4::encodeSerialFrame(payload.data(), payload.size(), frame.data()) == frame.size());
    frame[4] = static_cast<std::uint8_t>(frame[4] ^ 0xFFU); // flip a payload byte

    mark4::SerialFrameParser parser;
    std::array<std::size_t, 8> sizes{};
    REQUIRE(feedAll(parser, frame.data(), frame.size(), sizes) == 0U);
}

TEST_CASE("serial framing refuses empty and oversized payloads")
{
    std::array<std::uint8_t, 300> big{};
    std::array<std::uint8_t, 320> out{};
    REQUIRE(mark4::encodeSerialFrame(big.data(), 0U, out.data()) == 0U);
    REQUIRE(mark4::encodeSerialFrame(big.data(), big.size(), out.data()) == 0U);
}
