/// @file
/// @brief The boot metadata log: what a blank log reads as, what an append
///        persists, how the two areas ping-pong, and above all what a torn
///        record costs (nothing). Every state change of the update system is
///        one append here, so these are the tests behind the "power can drop
///        at any byte" claim of the design.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include "ota/crc32_mpeg2.hpp"
#include "ota/firmware_store.hpp"
#include "ota/meta_log.hpp"
#include "protocol/ota_image.hpp"

namespace
{
    /// One persisted record, decoded into ordinary aligned fields.
    struct Record
    {
        std::uint32_t counter = 0U;                                  ///< monotonic, newest wins
        std::uint8_t activeSlot = 0U;                                ///< preferred slot
        std::array<std::uint8_t, mark4::OTA_SLOT_COUNT> slotState{}; ///< OTA_SLOT_* per slot
        std::uint8_t flags = 0U;                                     ///< OTA_META_FLAG_* bits
        std::uint32_t crc = 0U;                                      ///< record checksum
    };

    /// Two erasable areas in RAM, the backend contract of OtaMetaLog. The
    /// area size is deliberately tiny: four records per area, so a ping-pong
    /// rollover is four appends away instead of a thousand.
    class RamMetaAreas
    {
      public:
        static constexpr std::uint32_t AREA_SIZE = 64U;
        static constexpr std::uint8_t AREA_COUNT = 2U;
        static constexpr std::uint32_t RECORDS_PER_AREA = AREA_SIZE / mark4::OTA_META_RECORD_SIZE;

        RamMetaAreas()
        {
            for (std::uint8_t area = 0U; area < AREA_COUNT; ++area)
            {
                m_bytes.at(area).fill(0xFFU);
            }
        }

        /// @brief Reads back a range of one area.
        /// @param area area index
        /// @param offset byte offset in the area
        /// @param[out] dataOut destination buffer
        /// @param size byte count
        /// @return false when out of bounds or when reads are set to fail
        bool read(std::uint8_t area,
                  std::uint32_t offset,
                  std::uint8_t *dataOut,
                  std::uint32_t size)
        {
            if (m_failRead || !InBounds(area, offset, size))
            {
                return false;
            }
            std::memcpy(dataOut, &m_bytes.at(area).at(offset), size);
            return true;
        }

        /// @brief Programs a blank range, like the flash underneath: a byte
        ///        that is not 0xFF refuses the write instead of taking it.
        /// @param area area index
        /// @param offset byte offset in the area
        /// @param data bytes to program
        /// @param size byte count
        /// @return false when out of bounds, not blank, or set to fail
        bool program(std::uint8_t area,
                     std::uint32_t offset,
                     const std::uint8_t *data,
                     std::uint32_t size)
        {
            if (m_failProgram || !InBounds(area, offset, size))
            {
                return false;
            }
            for (std::uint32_t i = 0U; i < size; ++i)
            {
                if (m_bytes.at(area).at(offset + i) != 0xFFU)
                {
                    return false;
                }
            }
            std::memcpy(&m_bytes.at(area).at(offset), data, size);
            ++m_programCount;
            return true;
        }

        /// @brief Erases one whole area back to 0xFF.
        /// @param area area index
        /// @return false when the area does not exist or erases are set to fail
        bool erase(std::uint8_t area)
        {
            if (m_failErase || area >= AREA_COUNT)
            {
                return false;
            }
            m_bytes.at(area).fill(0xFFU);
            ++m_eraseCount.at(area);
            return true;
        }

        /// @brief Overwrites one byte, blank or not: the power-cut hammer.
        /// @param area area index
        /// @param offset byte offset in the area
        /// @param value byte to store
        void poke(std::uint8_t area, std::uint32_t offset, std::uint8_t value)
        {
            m_bytes.at(area).at(offset) = value;
        }

        /// @param area area index
        /// @param index record index in the area
        /// @return the raw bytes of that record slot
        [[nodiscard]] std::array<std::uint8_t, mark4::OTA_META_RECORD_SIZE> recordBytesAt(
            std::uint8_t area, std::uint32_t index) const
        {
            std::array<std::uint8_t, mark4::OTA_META_RECORD_SIZE> bytes{};
            std::memcpy(bytes.data(),
                        &m_bytes.at(area).at(index * mark4::OTA_META_RECORD_SIZE),
                        bytes.size());
            return bytes;
        }

        /// @param area area index
        /// @param index record index in the area
        /// @return that record slot, decoded into aligned fields (the record
        ///         itself is byte-packed, so a test must never bind a
        ///         reference to one of its fields)
        [[nodiscard]] Record recordAt(std::uint8_t area, std::uint32_t index) const
        {
            mark4::OtaMetaRecord raw{};
            const std::array<std::uint8_t, mark4::OTA_META_RECORD_SIZE> bytes =
                recordBytesAt(area, index);
            std::memcpy(&raw, bytes.data(), bytes.size());

            Record record;
            record.counter = raw.counter;
            record.activeSlot = raw.activeSlot;
            std::memcpy(record.slotState.data(), &raw.slotState, mark4::OTA_SLOT_COUNT);
            record.flags = raw.flags;
            record.crc = raw.crc;
            return record;
        }

        /// @param area area index
        /// @param index record index in the area
        /// @return true when that record slot is still all 0xFF
        [[nodiscard]] bool blankAt(std::uint8_t area, std::uint32_t index) const
        {
            for (std::uint32_t i = 0U; i < mark4::OTA_META_RECORD_SIZE; ++i)
            {
                if (m_bytes.at(area).at((index * mark4::OTA_META_RECORD_SIZE) + i) != 0xFFU)
                {
                    return false;
                }
            }
            return true;
        }

        /// @param area area index
        /// @return how many times that area was erased
        [[nodiscard]] std::uint32_t eraseCount(std::uint8_t area) const
        {
            return m_eraseCount.at(area);
        }

        /// @return how many record writes reached the areas
        [[nodiscard]] std::uint32_t programCount() const
        {
            return m_programCount;
        }

        /// @brief Makes every subsequent read fail.
        void failReads()
        {
            m_failRead = true;
        }

        /// @brief Makes every subsequent program fail.
        void failPrograms()
        {
            m_failProgram = true;
        }

        /// @brief Makes every subsequent erase fail.
        void failErases()
        {
            m_failErase = true;
        }

      private:
        /// @param area area index
        /// @param offset byte offset in the area
        /// @param size byte count
        /// @return true when the range sits inside an existing area
        [[nodiscard]] static bool InBounds(std::uint8_t area,
                                           std::uint32_t offset,
                                           std::uint32_t size)
        {
            return area < AREA_COUNT && offset + size <= AREA_SIZE;
        }

        std::array<std::array<std::uint8_t, AREA_SIZE>, AREA_COUNT> m_bytes{}; ///< area contents
        std::array<std::uint32_t, AREA_COUNT> m_eraseCount{};                  ///< erases per area
        std::uint32_t m_programCount = 0U;                                     ///< record writes
        bool m_failRead = false;    ///< reads answer false from now on
        bool m_failProgram = false; ///< programs answer false from now on
        bool m_failErase = false;   ///< erases answer false from now on
    };

    /// @brief Builds a distinguishable logical state.
    /// @param activeSlot slot the bootloader should prefer
    /// @param stateA OTA_SLOT_* of slot A
    /// @param stateB OTA_SLOT_* of slot B
    /// @param attempted value of the trial-attempted flag
    /// @return the assembled state
    mark4::OtaMetaState makeState(std::uint8_t activeSlot,
                                  std::uint8_t stateA,
                                  std::uint8_t stateB,
                                  bool attempted)
    {
        mark4::OtaMetaState state;
        state.activeSlot = activeSlot;
        state.slotState = {stateA, stateB};
        state.trialAttempted = attempted;
        return state;
    }

    /// @brief Appends numbered states so each one is recognizable.
    /// @param log log to append to
    /// @param count how many records to append
    /// @param firstActive active slot of the first record, alternating after
    void appendSeveral(mark4::OtaMetaLog<RamMetaAreas> &log,
                       std::uint32_t count,
                       std::uint8_t firstActive)
    {
        for (std::uint32_t i = 0U; i < count; ++i)
        {
            const auto active =
                static_cast<std::uint8_t>((firstActive + i) % mark4::OTA_SLOT_COUNT);
            REQUIRE(log.append(
                makeState(active, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED, (i % 2U) == 1U)));
        }
    }
} // namespace

TEST_CASE("a blank metadata log reads as slot A active and trusted")
{
    RamMetaAreas areas;
    mark4::OtaMetaLog<RamMetaAreas> log(areas);

    mark4::OtaMetaState state = makeState(mark4::OTA_SLOT_B, mark4::OTA_SLOT_BAD, 0U, true);
    REQUIRE(log.read(state));

    REQUIRE(state.activeSlot == mark4::OTA_SLOT_A);
    REQUIRE(state.slotState[mark4::OTA_SLOT_A] == mark4::OTA_SLOT_VALID);
    REQUIRE(state.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    REQUIRE(!state.trialAttempted);
}

TEST_CASE("one append round trips through the log")
{
    RamMetaAreas areas;
    mark4::OtaMetaLog<RamMetaAreas> log(areas);

    const mark4::OtaMetaState written =
        makeState(mark4::OTA_SLOT_B, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_TESTING, true);
    REQUIRE(log.append(written));

    mark4::OtaMetaState read;
    REQUIRE(log.read(read));
    REQUIRE(read.activeSlot == written.activeSlot);
    REQUIRE(read.slotState == written.slotState);
    REQUIRE(read.trialAttempted == written.trialAttempted);

    // The persisted record carries its own CRC over the 12 bytes before it,
    // which is the whole torn-write defence.
    const Record record = areas.recordAt(0U, 0U);
    REQUIRE(record.counter == 1U);
    REQUIRE(record.flags == mark4::OTA_META_FLAG_TRIAL_ATTEMPTED);
    const std::array<std::uint8_t, mark4::OTA_META_RECORD_SIZE> bytes = areas.recordBytesAt(0U, 0U);
    REQUIRE(record.crc == mark4::crc32Mpeg2(bytes.data(), offsetof(mark4::OtaMetaRecord, crc)));
}

TEST_CASE("the counter grows by one per append and the newest record wins")
{
    RamMetaAreas areas;
    mark4::OtaMetaLog<RamMetaAreas> log(areas);

    appendSeveral(log, RamMetaAreas::RECORDS_PER_AREA, mark4::OTA_SLOT_A);

    for (std::uint32_t index = 0U; index < RamMetaAreas::RECORDS_PER_AREA; ++index)
    {
        REQUIRE(areas.recordAt(0U, index).counter == index + 1U);
    }
    REQUIRE(areas.eraseCount(0U) == 1U); // the defensive erase of a virgin area
    REQUIRE(areas.eraseCount(1U) == 0U);

    mark4::OtaMetaState state;
    REQUIRE(log.read(state));
    REQUIRE(state.activeSlot == mark4::OTA_SLOT_B); // the fourth record, zero-based
    REQUIRE(state.trialAttempted);
}

TEST_CASE("a full area ping-pongs into the other one, erased before it is used")
{
    RamMetaAreas areas;
    mark4::OtaMetaLog<RamMetaAreas> log(areas);

    // Leftovers of an earlier life in the other area: the rollover must wipe
    // them, or a stale record could outlive the log it belongs to.
    areas.poke(1U, 0U, 0x00U);
    areas.poke(1U, mark4::OTA_META_RECORD_SIZE, 0x00U);

    appendSeveral(log, RamMetaAreas::RECORDS_PER_AREA + 1U, mark4::OTA_SLOT_A);

    REQUIRE(areas.eraseCount(1U) == 1U);
    REQUIRE(areas.recordAt(1U, 0U).counter == RamMetaAreas::RECORDS_PER_AREA + 1U);
    for (std::uint32_t index = 1U; index < RamMetaAreas::RECORDS_PER_AREA; ++index)
    {
        REQUIRE(areas.blankAt(1U, index));
    }
    // The area left behind is untouched until it is needed again: there is
    // never a moment without a valid record somewhere.
    REQUIRE(areas.recordAt(0U, 0U).counter == 1U);

    mark4::OtaMetaState state;
    REQUIRE(log.read(state));
    REQUIRE(state.activeSlot == mark4::OTA_SLOT_A); // record five, zero-based index four
    REQUIRE(!state.trialAttempted);
}

TEST_CASE("coming back to the first area erases it, and the newest record survives")
{
    RamMetaAreas areas;
    mark4::OtaMetaLog<RamMetaAreas> log(areas);

    const std::uint32_t total = (2U * RamMetaAreas::RECORDS_PER_AREA) + 1U;
    appendSeveral(log, total, mark4::OTA_SLOT_A);

    REQUIRE(areas.eraseCount(0U) == 2U); // virgin erase, then the reuse
    REQUIRE(areas.eraseCount(1U) == 1U);
    REQUIRE(areas.recordAt(0U, 0U).counter == total);
    for (std::uint32_t index = 1U; index < RamMetaAreas::RECORDS_PER_AREA; ++index)
    {
        REQUIRE(areas.blankAt(0U, index));
    }

    mark4::OtaMetaState state;
    REQUIRE(log.read(state));
    REQUIRE(state.activeSlot == mark4::OTA_SLOT_A);
    REQUIRE(state.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_STAGED);
}

TEST_CASE("a torn newest record leaves the previous one in charge")
{
    RamMetaAreas areas;
    mark4::OtaMetaLog<RamMetaAreas> log(areas);

    REQUIRE(log.append(
        makeState(mark4::OTA_SLOT_A, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_EMPTY, false)));
    REQUIRE(log.append(
        makeState(mark4::OTA_SLOT_B, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED, true)));

    mark4::OtaMetaState state;
    REQUIRE(log.read(state));
    REQUIRE(state.activeSlot == mark4::OTA_SLOT_B);

    SECTION("a torn payload byte")
    {
        areas.poke(
            0U, mark4::OTA_META_RECORD_SIZE + offsetof(mark4::OtaMetaRecord, activeSlot), 0x07U);
    }
    SECTION("a torn counter")
    {
        areas.poke(0U, mark4::OTA_META_RECORD_SIZE, 0x55U);
    }
    SECTION("a half-written record, tail still blank")
    {
        for (std::uint32_t i = offsetof(mark4::OtaMetaRecord, reserved);
             i < mark4::OTA_META_RECORD_SIZE;
             ++i)
        {
            areas.poke(0U, mark4::OTA_META_RECORD_SIZE + i, 0xFFU);
        }
    }

    mark4::OtaMetaState recovered;
    REQUIRE(log.read(recovered));
    REQUIRE(recovered.activeSlot == mark4::OTA_SLOT_A);
    REQUIRE(recovered.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_EMPTY);
    REQUIRE(!recovered.trialAttempted);
}

TEST_CASE("a record slot once dirtied is never reused")
{
    RamMetaAreas areas;
    mark4::OtaMetaLog<RamMetaAreas> log(areas);

    REQUIRE(log.append(
        makeState(mark4::OTA_SLOT_A, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_EMPTY, false)));
    REQUIRE(log.append(
        makeState(mark4::OTA_SLOT_B, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED, false)));
    areas.poke(0U, mark4::OTA_META_RECORD_SIZE + offsetof(mark4::OtaMetaRecord, flags), 0x42U);

    const Record torn = areas.recordAt(0U, 1U);
    REQUIRE(log.append(
        makeState(mark4::OTA_SLOT_A, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_BAD, false)));

    // The torn bytes stayed exactly as they were, and the fresh record went
    // to the next slot: programming over a half-written record is what real
    // flash refuses, so the log must never try.
    REQUIRE(areas.recordAt(0U, 1U).flags == torn.flags);
    REQUIRE(areas.recordAt(0U, 1U).crc == torn.crc);
    REQUIRE(areas.recordAt(0U, 2U).counter == 2U); // the torn record's counter is gone with it

    mark4::OtaMetaState state;
    REQUIRE(log.read(state));
    REQUIRE(state.slotState[mark4::OTA_SLOT_B] == mark4::OTA_SLOT_BAD);
}

TEST_CASE("a backend that fails is reported, not papered over")
{
    SECTION("a failing read fails both read and append")
    {
        RamMetaAreas areas;
        mark4::OtaMetaLog<RamMetaAreas> log(areas);
        areas.failReads();

        mark4::OtaMetaState state;
        REQUIRE(!log.read(state));
        REQUIRE(!log.append(state));
    }
    SECTION("a failing program fails the append and writes nothing")
    {
        RamMetaAreas areas;
        mark4::OtaMetaLog<RamMetaAreas> log(areas);
        areas.failPrograms();

        REQUIRE(!log.append(
            makeState(mark4::OTA_SLOT_B, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED, false)));
        REQUIRE(areas.programCount() == 0U);
        REQUIRE(areas.blankAt(0U, 0U));
    }
    SECTION("a failing erase fails the very first append")
    {
        RamMetaAreas areas;
        mark4::OtaMetaLog<RamMetaAreas> log(areas);
        areas.failErases();

        REQUIRE(!log.append(
            makeState(mark4::OTA_SLOT_B, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED, false)));
        REQUIRE(areas.programCount() == 0U);
    }
    SECTION("a failing erase fails the rollover, and the newest record stands")
    {
        RamMetaAreas areas;
        mark4::OtaMetaLog<RamMetaAreas> log(areas);
        appendSeveral(log, RamMetaAreas::RECORDS_PER_AREA, mark4::OTA_SLOT_A);
        areas.failErases();

        REQUIRE(!log.append(
            makeState(mark4::OTA_SLOT_B, mark4::OTA_SLOT_VALID, mark4::OTA_SLOT_STAGED, false)));
        REQUIRE(areas.blankAt(1U, 0U));

        mark4::OtaMetaState state;
        REQUIRE(log.read(state));
        REQUIRE(state.activeSlot == mark4::OTA_SLOT_B); // the fourth record still
    }
}
