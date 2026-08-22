#pragma once

/// @file
/// @brief The board-side firmware update session, per docs/ota-design.md
///        sections 4.5 and 5: one packet in, at most one packet out, an
///        AbsFirmwareStore underneath and nothing else. It owns the whole
///        session state (nonce, target slot, next expected offset) and the
///        refusal policy, so the guarantees of the design are testable on a
///        desktop against a fake store instead of being promises about a
///        board.
///
///        Two invariants hold whatever arrives on the wire: the running slot
///        is never erased nor programmed (the target is always the other
///        one, and the store refuses it too), and every persisted state
///        change is a single metadata append (see
///        platform_common/ota_meta_log.hpp), which is what makes a power cut
///        at any byte harmless.
///
///        Board-safe: no allocation, no exceptions, one packet worth of
///        stack per call.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "platform/firmware_store.hpp"
#include "protocol/header.hpp"
#include "protocol/ota.hpp"

namespace mark4
{
    /// Consumes the OTA packets of protocol/ota.hpp and answers them. The
    /// surrounding app owns the transport: it hands over received bytes with
    /// the facts only it knows (armed, battery, clock) and sends back
    /// whatever reply comes out.
    class OtaUpdater
    {
      public:
        /// A session with no packet for this long is dropped. Long enough to
        /// ride out a retransmission round trip on a lossy WiFi link, short
        /// enough that a hub that walked away never leaves the board sitting
        /// in update mode.
        static constexpr std::uint64_t SESSION_TIMEOUT_US = 3000000U;

        /// Bytes of OtaImageHeader the updater ever reads: magic through git
        /// hash. Reading the prefix instead of the whole 512-byte header
        /// keeps one packet worth of stack the largest thing here.
        static constexpr std::uint32_t HEADER_PREFIX_SIZE = 28U;

        static_assert(HEADER_PREFIX_SIZE == offsetof(OtaImageHeader, reserved),
                      "the prefix must cover every field the updater reads");

        /// Facts only the surrounding app knows, refreshed per call.
        struct Inputs
        {
            bool armed = false;       ///< no update while the drone can spin motors
            bool voltageOk = true;    ///< battery above the update floor
            std::uint64_t nowUs = 0U; ///< monotonic time, session timeout base
        };

        /// @param store slot and metadata storage, not owned
        explicit OtaUpdater(AbsFirmwareStore &store)
            : m_store(store)
        {
        }

        /// @brief Feeds one received packet. At most one reply comes back.
        /// @param packet received bytes
        /// @param size received byte count
        /// @param in facts of the moment (armed, battery, clock)
        /// @param[out] replyOut buffer the reply is serialized into
        /// @param replyCapacity bytes available in replyOut
        /// @param[out] consumedOut true when the packet was an OTA packet,
        ///             whatever the outcome; false leaves it to the next
        ///             consumer of the same stream
        /// @return reply size written to replyOut, or 0 when there is none
        std::size_t handle(const std::uint8_t *packet,
                           std::size_t size,
                           const Inputs &in,
                           std::uint8_t *replyOut,
                           std::size_t replyCapacity,
                           bool &consumedOut)
        {
            consumedOut = false;
            if (packet == nullptr || size < 2U || packet[0] != PROTOCOL_VERSION)
            {
                return 0U;
            }

            // A packet shorter than its struct is not an OTA packet: it is
            // left unconsumed rather than answered, so a truncated frame
            // never looks like a refusal to the sender.
            switch (static_cast<PacketType>(packet[1]))
            {
                case PacketType::OTA_STATUS_REQUEST:
                    if (size < OTA_STATUS_REQUEST_PACKET_SIZE)
                    {
                        return 0U;
                    }
                    consumedOut = true;
                    return sendStatus(replyOut, replyCapacity);

                case PacketType::OTA_BEGIN:
                    if (size < OTA_BEGIN_PACKET_SIZE)
                    {
                        return 0U;
                    }
                    consumedOut = true;
                    return handleBegin(packet, in, replyOut, replyCapacity);

                case PacketType::OTA_CHUNK:
                    if (size < OTA_CHUNK_PACKET_SIZE)
                    {
                        return 0U;
                    }
                    consumedOut = true;
                    return handleChunk(packet, in, replyOut, replyCapacity);

                case PacketType::OTA_FINISH:
                    if (size < OTA_FINISH_PACKET_SIZE)
                    {
                        return 0U;
                    }
                    consumedOut = true;
                    return handleFinish(packet, in, replyOut, replyCapacity);

                case PacketType::OTA_CONFIRM:
                    if (size < OTA_CONFIRM_PACKET_SIZE)
                    {
                        return 0U;
                    }
                    consumedOut = true;
                    return handleConfirm(replyOut, replyCapacity);

                case PacketType::OTA_REVERT:
                    if (size < OTA_REVERT_PACKET_SIZE)
                    {
                        return 0U;
                    }
                    consumedOut = true;
                    return handleRevert(replyOut, replyCapacity);

                case PacketType::OTA_ABORT:
                    if (size < OTA_ABORT_PACKET_SIZE)
                    {
                        return 0U;
                    }
                    consumedOut = true;
                    return handleAbort(packet, replyOut, replyCapacity);

                default:
                    return 0U;
            }
        }

        /// @brief Session timeout sweep; call once per main-loop turn. A
        ///        session whose sender went silent is dropped without a
        ///        reply: there is nobody left to tell.
        /// @param nowUs monotonic time [us]
        void tick(std::uint64_t nowUs)
        {
            if (!m_sessionActive || nowUs < m_lastPacketUs)
            {
                return;
            }
            if ((nowUs - m_lastPacketUs) >= SESSION_TIMEOUT_US)
            {
                closeSession();
            }
        }

        /// @return true while a transfer session is open
        [[nodiscard]] bool sessionActive() const
        {
            return m_sessionActive;
        }

      private:
        /// @return the slot this firmware executes from, normalized to an
        ///         existing slot so nothing downstream can index out of the
        ///         two-slot arrays
        [[nodiscard]] std::uint8_t runningSlot() const
        {
            return (m_store.runningSlot() == OTA_SLOT_B) ? OTA_SLOT_B : OTA_SLOT_A;
        }

        /// @return the slot an update may target: the one not running. Never
        ///         the metadata's inactive slot, which during a trial boot is
        ///         the running one.
        [[nodiscard]] std::uint8_t targetSlot() const
        {
            return (runningSlot() == OTA_SLOT_A) ? OTA_SLOT_B : OTA_SLOT_A;
        }

        /// @brief Serializes one wire struct into the reply buffer.
        /// @tparam Packet wire struct type
        /// @param packet packet to send
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return bytes written, 0 when the buffer cannot hold the packet
        template <typename Packet>
        static std::size_t Emit(const Packet &packet,
                                std::uint8_t *replyOut,
                                std::size_t replyCapacity)
        {
            if (replyOut == nullptr || replyCapacity < sizeof(Packet))
            {
                return 0U;
            }
            std::memcpy(replyOut, &packet, sizeof(Packet));
            return sizeof(Packet);
        }

        /// @brief Answers one request with the single ack packet of the
        ///        updater.
        /// @param ackedType request being answered
        /// @param session session nonce, 0 when none applies
        /// @param result OTA_RESULT_*
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        static std::size_t Ack(PacketType ackedType,
                               std::uint32_t session,
                               std::uint8_t result,
                               std::uint8_t *replyOut,
                               std::size_t replyCapacity)
        {
            OtaAckPacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::OTA_ACK);
            packet.session = session;
            packet.ackedType = static_cast<std::uint8_t>(ackedType);
            packet.result = result;
            return Emit(packet, replyOut, replyCapacity);
        }

        /// @brief Answers with the cumulative chunk acknowledgement, which
        ///        doubles as the go-back-N resume point.
        /// @param session session nonce
        /// @param nextOffset first image byte still missing
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        static std::size_t ChunkAck(std::uint32_t session,
                                    std::uint32_t nextOffset,
                                    std::uint8_t *replyOut,
                                    std::size_t replyCapacity)
        {
            OtaChunkAckPacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::OTA_CHUNK_ACK);
            packet.session = session;
            packet.nextOffset = nextOffset;
            return Emit(packet, replyOut, replyCapacity);
        }

        /// @brief Reads the image header prefix of a slot.
        /// @param slot slot to read from
        /// @param[out] headerOut header fields, valid only on true; the
        ///             fields past the prefix stay zeroed
        /// @return false on a store read failure
        [[nodiscard]] bool readImageHeader(std::uint8_t slot, OtaImageHeader &headerOut) const
        {
            std::array<std::uint8_t, HEADER_PREFIX_SIZE> bytes{};
            if (!m_store.read(slot, 0U, bytes.data(), HEADER_PREFIX_SIZE))
            {
                return false;
            }
            std::memcpy(&headerOut, bytes.data(), bytes.size());
            return true;
        }

        /// @brief Answers a status request; legal at any time, session or
        ///        not, and never refused.
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        std::size_t sendStatus(std::uint8_t *replyOut, std::size_t replyCapacity) const
        {
            OtaStatusPacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::OTA_STATUS);
            packet.mcuId = m_store.mcuId();
            packet.runningSlot = runningSlot();
            packet.updaterBusy = m_sessionActive ? 1U : 0U;
            packet.slotSize = m_store.slotSize();
            packet.maxChunkData = static_cast<std::uint16_t>(OTA_CHUNK_DATA_SIZE);

            // The slot states come from the metadata and are reported raw,
            // running slot included: a TESTING running slot is exactly how
            // the sender learns a trial boot is in progress. A store that
            // cannot even be read claims nothing.
            OtaMetaState meta;
            const std::array<std::uint8_t, OTA_SLOT_COUNT> unknown = {OTA_SLOT_EMPTY,
                                                                      OTA_SLOT_EMPTY};
            const bool metaRead = m_store.readMeta(meta);
            std::memcpy(&packet.slotState,
                        metaRead ? meta.slotState.data() : unknown.data(),
                        OTA_SLOT_COUNT);
            // The active slot differs from the running one during a trial
            // boot and after a revert; an unreadable store falls back to
            // the one fact that needs no metadata.
            packet.activeSlot = metaRead ? meta.activeSlot : packet.runningSlot;

            // The version fields are compile-time facts of the running
            // image, so they are reported whether or not the packaging step
            // ever stamped a CRC over them; an image with no header at all
            // (or an unreadable slot) reports nothing instead of garbage.
            OtaImageHeader header{};
            if (readImageHeader(runningSlot(), header) && header.magic == OTA_IMAGE_MAGIC)
            {
                packet.versionMajor = header.versionMajor;
                packet.versionMinor = header.versionMinor;
                packet.versionPatch = header.versionPatch;
                std::memcpy(&packet.gitHash, header.gitHash.data(), OTA_GIT_HASH_SIZE);
            }
            return Emit(packet, replyOut, replyCapacity);
        }

        /// @brief Opens a session into the target slot, erasing it first.
        /// @param packet received bytes, at least OTA_BEGIN_PACKET_SIZE
        /// @param in facts of the moment
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        std::size_t handleBegin(const std::uint8_t *packet,
                                const Inputs &in,
                                std::uint8_t *replyOut,
                                std::size_t replyCapacity)
        {
            OtaBeginPacket request{};
            std::memcpy(&request, packet, sizeof(request));

            if (in.armed)
            {
                return Ack(
                    PacketType::OTA_BEGIN, 0U, OTA_RESULT_DENIED_ARMED, replyOut, replyCapacity);
            }
            if (!in.voltageOk)
            {
                return Ack(
                    PacketType::OTA_BEGIN, 0U, OTA_RESULT_DENIED_VOLTAGE, replyOut, replyCapacity);
            }
            // A retry of the same begin restarts the same session from
            // scratch: the sender that lost the first ack must be able to
            // ask again. Another nonce while one session is open is the hub
            // stepping on itself and is refused.
            if (m_sessionActive && request.session != m_session)
            {
                return Ack(
                    PacketType::OTA_BEGIN, 0U, OTA_RESULT_DENIED_BUSY, replyOut, replyCapacity);
            }
            if (request.imageSize < OTA_IMAGE_HEADER_SIZE || request.imageSize > m_store.slotSize())
            {
                return Ack(
                    PacketType::OTA_BEGIN, 0U, OTA_RESULT_BAD_IMAGE, replyOut, replyCapacity);
            }

            // Anything below can fail halfway: the session is dropped first
            // so a failure never leaves stale transfer state behind.
            closeSession();
            const std::uint8_t target = targetSlot();

            // The metadata is written before the erase, not after: it says
            // the target slot holds nothing the moment its content stops
            // being trustworthy. A power cut during the erase then finds a
            // record that already tells the truth, and the fallback image
            // the target used to hold is never claimed to exist.
            OtaMetaState meta;
            if (!m_store.readMeta(meta))
            {
                return Ack(
                    PacketType::OTA_BEGIN, 0U, OTA_RESULT_STORE_FAILURE, replyOut, replyCapacity);
            }
            meta.slotState[target] = OTA_SLOT_EMPTY;
            if (!m_store.writeMeta(meta) || !m_store.eraseSlot(target))
            {
                return Ack(
                    PacketType::OTA_BEGIN, 0U, OTA_RESULT_STORE_FAILURE, replyOut, replyCapacity);
            }

            m_sessionActive = true;
            m_session = request.session;
            m_targetSlot = target;
            m_imageSize = request.imageSize;
            m_imageCrc = request.imageCrc;
            m_nextOffset = 0U;
            m_chunksSinceAck = 0U;
            m_lastPacketUs = in.nowUs;
            return Ack(PacketType::OTA_BEGIN, m_session, OTA_RESULT_OK, replyOut, replyCapacity);
        }

        /// @brief Programs one in-order chunk, or repeats what is expected.
        /// @param packet received bytes, at least OTA_CHUNK_PACKET_SIZE
        /// @param in facts of the moment
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes, 0 when the chunk is simply absorbed
        std::size_t handleChunk(const std::uint8_t *packet,
                                const Inputs &in,
                                std::uint8_t *replyOut,
                                std::size_t replyCapacity)
        {
            OtaChunkPacket request{};
            std::memcpy(&request, packet, sizeof(request));

            // A chunk of an abandoned session is dropped in silence: the
            // sender has no session to be told about, and answering would
            // let stale traffic drive the current one.
            if (!m_sessionActive || request.session != m_session)
            {
                return 0U;
            }
            m_lastPacketUs = in.nowUs;

            const std::uint32_t length = request.length;
            const bool sane = length > 0U && length <= OTA_CHUNK_DATA_SIZE &&
                              request.offset <= m_imageSize &&
                              (m_imageSize - request.offset) >= length;
            if (!sane || request.offset != m_nextOffset)
            {
                // Duplicate, out of order or out of bounds: repeating the
                // expected offset is the whole retransmission protocol.
                return ChunkAck(m_session, m_nextOffset, replyOut, replyCapacity);
            }

            if (!m_store.program(m_targetSlot, request.offset, request.data.data(), length))
            {
                const std::uint32_t session = m_session;
                closeSession();
                return Ack(PacketType::OTA_CHUNK,
                           session,
                           OTA_RESULT_STORE_FAILURE,
                           replyOut,
                           replyCapacity);
            }
            m_nextOffset += length;
            ++m_chunksSinceAck;

            // The last byte of the image is acknowledged whatever the window
            // position: that ack is what tells the sender to finish.
            if (m_nextOffset >= m_imageSize || m_chunksSinceAck >= OTA_CHUNK_ACK_WINDOW)
            {
                m_chunksSinceAck = 0U;
                return ChunkAck(m_session, m_nextOffset, replyOut, replyCapacity);
            }
            return 0U;
        }

        /// @brief Verifies the transferred image and stages it.
        /// @param packet received bytes, at least OTA_FINISH_PACKET_SIZE
        /// @param in facts of the moment
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        std::size_t handleFinish(const std::uint8_t *packet,
                                 const Inputs &in,
                                 std::uint8_t *replyOut,
                                 std::size_t replyCapacity)
        {
            OtaFinishPacket request{};
            std::memcpy(&request, packet, sizeof(request));

            if (!m_sessionActive || request.session != m_session)
            {
                return Ack(PacketType::OTA_FINISH,
                           request.session,
                           OTA_RESULT_BAD_SESSION,
                           replyOut,
                           replyCapacity);
            }
            m_lastPacketUs = in.nowUs;

            // Bytes still missing: answered with the chunk acknowledgement
            // rather than a refusal, so the sender resumes the tail instead
            // of restarting the transfer.
            if (m_nextOffset < m_imageSize)
            {
                return ChunkAck(m_session, m_nextOffset, replyOut, replyCapacity);
            }

            const std::uint32_t session = m_session;
            if (m_store.crc32(m_targetSlot, 0U, m_imageSize) != m_imageCrc)
            {
                // The slot keeps whatever it holds and stays untrusted: no
                // metadata record means the bootloader ignores it.
                closeSession();
                return Ack(PacketType::OTA_FINISH,
                           session,
                           OTA_RESULT_CRC_MISMATCH,
                           replyOut,
                           replyCapacity);
            }

            OtaImageHeader header{};
            if (!readImageHeader(m_targetSlot, header) || header.magic != OTA_IMAGE_MAGIC ||
                header.headerVersion != OTA_IMAGE_HEADER_VERSION ||
                header.mcuId != m_store.mcuId() || header.slotId != m_targetSlot ||
                header.imageSize != m_imageSize)
            {
                closeSession();
                return Ack(
                    PacketType::OTA_FINISH, session, OTA_RESULT_BAD_IMAGE, replyOut, replyCapacity);
            }

            // Staging is one record: the target becomes STAGED, the active
            // slot does not move (the bootloader picks the staged image for
            // exactly one trial boot) and the trial flag starts clean.
            OtaMetaState meta;
            if (!m_store.readMeta(meta))
            {
                return Ack(PacketType::OTA_FINISH,
                           session,
                           OTA_RESULT_STORE_FAILURE,
                           replyOut,
                           replyCapacity);
            }
            meta.slotState[m_targetSlot] = OTA_SLOT_STAGED;
            meta.trialAttempted = false;
            if (!m_store.writeMeta(meta))
            {
                // The session stays open on purpose: every byte is in flash
                // and verified, so a retried finish can still stage it.
                return Ack(PacketType::OTA_FINISH,
                           session,
                           OTA_RESULT_STORE_FAILURE,
                           replyOut,
                           replyCapacity);
            }

            closeSession();
            return Ack(PacketType::OTA_FINISH, session, OTA_RESULT_OK, replyOut, replyCapacity);
        }

        /// @brief Confirms the running trial image, sessionless.
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        std::size_t handleConfirm(std::uint8_t *replyOut, std::size_t replyCapacity) const
        {
            OtaMetaState meta;
            if (!m_store.readMeta(meta))
            {
                return Ack(
                    PacketType::OTA_CONFIRM, 0U, OTA_RESULT_STORE_FAILURE, replyOut, replyCapacity);
            }
            // Only a trial can be confirmed: confirming anything else would
            // be the ground side claiming something it never observed.
            if (meta.slotState[runningSlot()] != OTA_SLOT_TESTING)
            {
                return Ack(
                    PacketType::OTA_CONFIRM, 0U, OTA_RESULT_BAD_STATE, replyOut, replyCapacity);
            }
            meta.slotState[runningSlot()] = OTA_SLOT_VALID;
            meta.activeSlot = runningSlot();
            meta.trialAttempted = false;
            if (!m_store.writeMeta(meta))
            {
                return Ack(
                    PacketType::OTA_CONFIRM, 0U, OTA_RESULT_STORE_FAILURE, replyOut, replyCapacity);
            }
            return Ack(PacketType::OTA_CONFIRM, 0U, OTA_RESULT_OK, replyOut, replyCapacity);
        }

        /// @brief Points the bootloader back at the other slot, sessionless.
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        std::size_t handleRevert(std::uint8_t *replyOut, std::size_t replyCapacity) const
        {
            OtaMetaState meta;
            if (!m_store.readMeta(meta))
            {
                return Ack(
                    PacketType::OTA_REVERT, 0U, OTA_RESULT_STORE_FAILURE, replyOut, replyCapacity);
            }
            // The rollback target is the slot that is not running, and
            // reverting onto anything but a VALID image would hand the
            // bootloader a slot nobody vouched for.
            const std::uint8_t rollback = targetSlot();
            if (meta.slotState[rollback] != OTA_SLOT_VALID)
            {
                return Ack(
                    PacketType::OTA_REVERT, 0U, OTA_RESULT_BAD_STATE, replyOut, replyCapacity);
            }
            meta.activeSlot = rollback;
            if (!m_store.writeMeta(meta))
            {
                return Ack(
                    PacketType::OTA_REVERT, 0U, OTA_RESULT_STORE_FAILURE, replyOut, replyCapacity);
            }
            return Ack(PacketType::OTA_REVERT, 0U, OTA_RESULT_OK, replyOut, replyCapacity);
        }

        /// @brief Drops the matching session; the slot is left as it is.
        /// @param packet received bytes, at least OTA_ABORT_PACKET_SIZE
        /// @param[out] replyOut destination buffer
        /// @param replyCapacity bytes available in replyOut
        /// @return reply size in bytes
        std::size_t handleAbort(const std::uint8_t *packet,
                                std::uint8_t *replyOut,
                                std::size_t replyCapacity)
        {
            OtaAbortPacket request{};
            std::memcpy(&request, packet, sizeof(request));

            if (!m_sessionActive || request.session != m_session)
            {
                return Ack(PacketType::OTA_ABORT,
                           request.session,
                           OTA_RESULT_BAD_SESSION,
                           replyOut,
                           replyCapacity);
            }
            const std::uint32_t session = m_session;
            closeSession();
            return Ack(PacketType::OTA_ABORT, session, OTA_RESULT_OK, replyOut, replyCapacity);
        }

        /// @brief Forgets the session. The target slot is left exactly as it
        ///        is: only a metadata record ever makes flash content
        ///        bootable, and an aborted transfer wrote none.
        void closeSession()
        {
            m_sessionActive = false;
            m_imageSize = 0U;
            m_imageCrc = 0U;
            m_nextOffset = 0U;
            m_chunksSinceAck = 0U;
        }

        AbsFirmwareStore &m_store;           ///< slot and metadata storage, not owned
        bool m_sessionActive = false;        ///< a transfer session is open
        std::uint32_t m_session = 0U;        ///< nonce of the open session
        std::uint8_t m_targetSlot = 0U;      ///< slot being written, never the running one
        std::uint32_t m_imageSize = 0U;      ///< image bytes announced at begin
        std::uint32_t m_imageCrc = 0U;       ///< image CRC announced at begin
        std::uint32_t m_nextOffset = 0U;     ///< first image byte still missing
        std::uint32_t m_chunksSinceAck = 0U; ///< in-order chunks since the last ack
        std::uint64_t m_lastPacketUs = 0U;   ///< arrival time of the last session packet [us]
    };
} // namespace mark4
