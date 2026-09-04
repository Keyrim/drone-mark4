#pragma once

/// @file
/// @brief The board-side firmware update session, per docs/ota-design.md
///        sections 4.5 and 5: one message in, at most one message out, an
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
///        ota/meta_log.hpp), which is what makes a power cut
///        at any byte harmless.
///
///        Board-safe: no allocation, no exceptions, one message worth of
///        stack per call.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ota/firmware_store.hpp"
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"

namespace mark4
{
    /// Consumes the Ota* messages of the wire and answers them. The
    /// surrounding app owns the link: it hands over decoded messages with
    /// the facts only it knows (armed, battery, clock) and sends back
    /// whatever reply comes out.
    class OtaUpdater
    {
      public:
        /// A session with no message for this long is dropped. Long enough to
        /// ride out a retransmission round trip on a lossy WiFi link, short
        /// enough that a hub that walked away never leaves the board sitting
        /// in update mode.
        static constexpr std::uint64_t SESSION_TIMEOUT_US = 3000000U;

        /// Bytes of OtaImageHeader the updater ever reads: magic through git
        /// hash. Reading the prefix instead of the whole 512-byte header
        /// keeps one message worth of stack the largest thing here.
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
        /// @param selfConfirmOnContact true (the default, and what the real
        ///        firmware wants) makes a trial image confirm itself on the
        ///        first valid ground request it serves: by then the image
        ///        has booted, run its loop and proven the receive side of
        ///        the radio, which is everything the next update needs.
        ///        false keeps the trial pending, for a host faking an image
        ///        that never reaches that point.
        explicit OtaUpdater(AbsFirmwareStore &store, bool selfConfirmOnContact = true)
            : m_store(store),
              m_selfConfirmOnContact(selfConfirmOnContact)
        {
        }

        /// @brief Feeds one received message. At most one reply comes back.
        /// @param request decoded message
        /// @param in facts of the moment (armed, battery, clock)
        /// @param[out] replyOut the reply, zeroed first; which_body stays 0
        ///             when there is none
        /// @return true when the message was an updater message, whatever the
        ///         outcome; false leaves it to the next consumer of the stream
        bool handle(const mark4_Envelope &request, const Inputs &in, mark4_Envelope &replyOut)
        {
            replyOut = mark4_Envelope_init_zero;
            switch (request.which_body)
            {
                case mark4_Envelope_ota_status_request_tag:
                    // A trial image confirms itself on its first ground
                    // contact: serving this request proves boot, loop and
                    // radio reception, the checkpoint that matters for the
                    // next update. The reply then already reports VALID.
                    if (m_selfConfirmOnContact)
                    {
                        selfConfirmTrial();
                    }
                    sendStatus(replyOut);
                    return true;
                case mark4_Envelope_ota_begin_tag:
                    handleBegin(request.body.ota_begin, in, replyOut);
                    return true;
                case mark4_Envelope_ota_chunk_tag:
                    handleChunk(request.body.ota_chunk, in, replyOut);
                    return true;
                case mark4_Envelope_ota_finish_tag:
                    handleFinish(request.body.ota_finish, in, replyOut);
                    return true;
                case mark4_Envelope_ota_revert_tag:
                    handleRevert(replyOut);
                    return true;
                case mark4_Envelope_ota_abort_tag:
                    handleAbort(request.body.ota_abort, replyOut);
                    return true;
                default:
                    return false;
            }
        }

        /// @brief Session timeout sweep; call once per main-loop turn. A
        ///        session whose sender went silent is dropped without a
        ///        reply: there is nobody left to tell.
        /// @param nowUs monotonic time [us]
        void tick(std::uint64_t nowUs)
        {
            if (!m_sessionActive)
            {
                return;
            }
            if (m_sessionFresh)
            {
                // The begin was stamped before its own erase, and a slot
                // erase freezes a flash-resident core for seconds: judging
                // the fresh session against that stale stamp killed it on
                // the first bench transfer. The first sweep after the open
                // re-arms the clock instead of comparing.
                m_sessionFresh = false;
                m_lastPacketUs = nowUs;
                return;
            }
            if (nowUs < m_lastPacketUs)
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

        /// @brief Fills the single ack message of the updater.
        /// @param op request being answered
        /// @param session session nonce, 0 when none applies
        /// @param result outcome
        /// @param[out] replyOut message to fill
        static void Ack(mark4_OtaOp op,
                        std::uint32_t session,
                        mark4_OtaResult result,
                        mark4_Envelope &replyOut)
        {
            replyOut.which_body = mark4_Envelope_ota_ack_tag;
            replyOut.body.ota_ack.session = session;
            replyOut.body.ota_ack.op = op;
            replyOut.body.ota_ack.result = result;
        }

        /// @brief Fills the cumulative chunk acknowledgement, which doubles
        ///        as the go-back-N resume point.
        /// @param session session nonce
        /// @param nextOffset first image byte still missing
        /// @param[out] replyOut message to fill
        static void ChunkAck(std::uint32_t session,
                             std::uint32_t nextOffset,
                             mark4_Envelope &replyOut)
        {
            replyOut.which_body = mark4_Envelope_ota_chunk_ack_tag;
            replyOut.body.ota_chunk_ack.session = session;
            replyOut.body.ota_chunk_ack.next_offset = nextOffset;
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
        /// @param[out] replyOut message to fill
        void sendStatus(mark4_Envelope &replyOut) const
        {
            replyOut.which_body = mark4_Envelope_ota_status_tag;
            mark4_OtaStatus &status = replyOut.body.ota_status;
            status.mcu = static_cast<mark4_Mcu>(m_store.mcuId());
            status.running_slot = runningSlot();
            status.updater_busy = m_sessionActive;
            status.slot_size = m_store.slotSize();
            status.max_chunk_data = static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE);

            // The slot states come from the metadata and are reported raw,
            // running slot included: a TESTING running slot is exactly how
            // the sender learns a trial boot is in progress. A store that
            // cannot even be read claims nothing.
            OtaMetaState meta;
            const bool metaRead = m_store.readMeta(meta);
            // The active slot differs from the running one during a trial
            // boot and after a revert; an unreadable store falls back to
            // the one fact that needs no metadata.
            status.active_slot = metaRead ? meta.activeSlot : status.running_slot;

            for (std::uint8_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
            {
                mark4_OtaSlotStatus &out = status.slots[slot];
                out.state = otaSlotStateToWire(metaRead ? meta.slotState[slot] : OTA_SLOT_EMPTY);
                // The identity is reported raw out of each slot's image
                // header, stamped or not (an unstamped image says
                // OTA_IMAGE_UNSTAMPED); a slot with no header at all (or
                // an unreadable one) reports nothing instead of garbage.
                OtaImageHeader header{};
                if (readImageHeader(slot, header) && header.magic == OTA_IMAGE_MAGIC)
                {
                    out.build_epoch = header.buildEpoch;
                    std::array<char, OTA_GIT_HASH_SIZE> hash{};
                    std::memcpy(hash.data(), &header.gitHash, OTA_GIT_HASH_SIZE);
                    otaGitHashToWire(hash, out.git_hash);
                }
            }
        }

        /// @brief Opens a session into the target slot, erasing it first.
        /// @param request decoded begin
        /// @param in facts of the moment
        /// @param[out] replyOut message to fill
        void handleBegin(const mark4_OtaBegin &request, const Inputs &in, mark4_Envelope &replyOut)
        {
            if (in.armed)
            {
                Ack(mark4_OtaOp_BEGIN, 0U, mark4_OtaResult_DENIED_ARMED, replyOut);
                return;
            }
            if (!in.voltageOk)
            {
                Ack(mark4_OtaOp_BEGIN, 0U, mark4_OtaResult_DENIED_VOLTAGE, replyOut);
                return;
            }
            // A retry of the same begin restarts the same session from
            // scratch: the sender that lost the first ack must be able to
            // ask again. Another nonce while one session is open is the hub
            // stepping on itself and is refused.
            if (m_sessionActive && request.session != m_session)
            {
                Ack(mark4_OtaOp_BEGIN, 0U, mark4_OtaResult_DENIED_BUSY, replyOut);
                return;
            }
            if (request.image_size < OTA_IMAGE_HEADER_SIZE ||
                request.image_size > m_store.slotSize())
            {
                Ack(mark4_OtaOp_BEGIN, 0U, mark4_OtaResult_BAD_IMAGE, replyOut);
                return;
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
                Ack(mark4_OtaOp_BEGIN, 0U, mark4_OtaResult_STORE_FAILURE, replyOut);
                return;
            }
            meta.slotState[target] = OTA_SLOT_EMPTY;
            if (!m_store.writeMeta(meta) || !m_store.eraseSlot(target))
            {
                Ack(mark4_OtaOp_BEGIN, 0U, mark4_OtaResult_STORE_FAILURE, replyOut);
                return;
            }

            m_sessionActive = true;
            m_sessionFresh = true;
            m_session = request.session;
            m_targetSlot = target;
            m_imageSize = request.image_size;
            m_imageCrc = request.image_crc;
            m_nextOffset = 0U;
            m_chunksSinceAck = 0U;
            m_lastPacketUs = in.nowUs;
            Ack(mark4_OtaOp_BEGIN, m_session, mark4_OtaResult_OTA_OK, replyOut);
        }

        /// @brief Programs one in-order chunk, or repeats what is expected.
        /// @param request decoded chunk
        /// @param in facts of the moment
        /// @param[out] replyOut message to fill, left empty when the chunk
        ///             is simply absorbed
        void handleChunk(const mark4_OtaChunk &request, const Inputs &in, mark4_Envelope &replyOut)
        {
            // A chunk of an abandoned session is dropped in silence: the
            // sender has no session to be told about, and answering would
            // let stale traffic drive the current one.
            if (!m_sessionActive || request.session != m_session)
            {
                return;
            }
            m_lastPacketUs = in.nowUs;

            const std::uint32_t length = request.data.size;
            const bool sane = length > 0U && length <= OTA_CHUNK_DATA_SIZE &&
                              request.offset <= m_imageSize &&
                              (m_imageSize - request.offset) >= length;
            if (!sane || request.offset != m_nextOffset)
            {
                // Duplicate, out of order or out of bounds: repeating the
                // expected offset is the whole retransmission protocol.
                ChunkAck(m_session, m_nextOffset, replyOut);
                return;
            }

            if (!m_store.program(m_targetSlot, request.offset, request.data.bytes, length))
            {
                const std::uint32_t session = m_session;
                closeSession();
                Ack(mark4_OtaOp_CHUNK, session, mark4_OtaResult_STORE_FAILURE, replyOut);
                return;
            }
            m_nextOffset += length;
            ++m_chunksSinceAck;

            // The last byte of the image is acknowledged whatever the window
            // position: that ack is what tells the sender to finish.
            if (m_nextOffset >= m_imageSize || m_chunksSinceAck >= OTA_CHUNK_ACK_WINDOW)
            {
                m_chunksSinceAck = 0U;
                ChunkAck(m_session, m_nextOffset, replyOut);
            }
        }

        /// @brief Verifies the transferred image and stages it.
        /// @param request decoded finish
        /// @param in facts of the moment
        /// @param[out] replyOut message to fill
        void handleFinish(const mark4_OtaFinish &request,
                          const Inputs &in,
                          mark4_Envelope &replyOut)
        {
            if (!m_sessionActive || request.session != m_session)
            {
                Ack(mark4_OtaOp_FINISH, request.session, mark4_OtaResult_BAD_SESSION, replyOut);
                return;
            }
            m_lastPacketUs = in.nowUs;

            // Bytes still missing: answered with the chunk acknowledgement
            // rather than a refusal, so the sender resumes the tail instead
            // of restarting the transfer.
            if (m_nextOffset < m_imageSize)
            {
                ChunkAck(m_session, m_nextOffset, replyOut);
                return;
            }

            const std::uint32_t session = m_session;
            if (m_store.crc32(m_targetSlot, 0U, m_imageSize) != m_imageCrc)
            {
                // The slot keeps whatever it holds and stays untrusted: no
                // metadata record means the bootloader ignores it.
                closeSession();
                Ack(mark4_OtaOp_FINISH, session, mark4_OtaResult_CRC_MISMATCH, replyOut);
                return;
            }

            OtaImageHeader header{};
            if (!readImageHeader(m_targetSlot, header) || header.magic != OTA_IMAGE_MAGIC ||
                header.headerVersion != OTA_IMAGE_HEADER_VERSION ||
                header.mcuId != m_store.mcuId() || header.slotId != m_targetSlot ||
                header.imageSize != m_imageSize)
            {
                closeSession();
                Ack(mark4_OtaOp_FINISH, session, mark4_OtaResult_BAD_IMAGE, replyOut);
                return;
            }

            // Staging is one record: the target becomes STAGED, the active
            // slot does not move (the bootloader picks the staged image for
            // exactly one trial boot) and the trial flag starts clean.
            OtaMetaState meta;
            if (!m_store.readMeta(meta))
            {
                Ack(mark4_OtaOp_FINISH, session, mark4_OtaResult_STORE_FAILURE, replyOut);
                return;
            }
            meta.slotState[m_targetSlot] = OTA_SLOT_STAGED;
            meta.trialAttempted = false;
            if (!m_store.writeMeta(meta))
            {
                // The session stays open on purpose: every byte is in flash
                // and verified, so a retried finish can still stage it.
                Ack(mark4_OtaOp_FINISH, session, mark4_OtaResult_STORE_FAILURE, replyOut);
                return;
            }

            closeSession();
            Ack(mark4_OtaOp_FINISH, session, mark4_OtaResult_OTA_OK, replyOut);
        }

        /// @brief Marks a running trial VALID, the firmware vouching for
        ///        itself. A failed meta read or write changes nothing: the
        ///        trial stays pending and the next request tries again.
        void selfConfirmTrial() const
        {
            OtaMetaState meta;
            if (!m_store.readMeta(meta) || meta.slotState[runningSlot()] != OTA_SLOT_TESTING)
            {
                return;
            }
            meta.slotState[runningSlot()] = OTA_SLOT_VALID;
            meta.activeSlot = runningSlot();
            meta.trialAttempted = false;
            static_cast<void>(m_store.writeMeta(meta));
        }

        /// @brief Points the bootloader back at the other slot, sessionless.
        /// @param[out] replyOut message to fill
        void handleRevert(mark4_Envelope &replyOut) const
        {
            OtaMetaState meta;
            if (!m_store.readMeta(meta))
            {
                Ack(mark4_OtaOp_REVERT, 0U, mark4_OtaResult_STORE_FAILURE, replyOut);
                return;
            }
            // The rollback target is the slot that is not running, and
            // reverting onto anything but a VALID image would hand the
            // bootloader a slot nobody vouched for.
            const std::uint8_t rollback = targetSlot();
            if (meta.slotState[rollback] != OTA_SLOT_VALID)
            {
                Ack(mark4_OtaOp_REVERT, 0U, mark4_OtaResult_BAD_STATE, replyOut);
                return;
            }
            meta.activeSlot = rollback;
            if (!m_store.writeMeta(meta))
            {
                Ack(mark4_OtaOp_REVERT, 0U, mark4_OtaResult_STORE_FAILURE, replyOut);
                return;
            }
            Ack(mark4_OtaOp_REVERT, 0U, mark4_OtaResult_OTA_OK, replyOut);
        }

        /// @brief Drops the matching session; the slot is left as it is.
        /// @param request decoded abort
        /// @param[out] replyOut message to fill
        void handleAbort(const mark4_OtaAbort &request, mark4_Envelope &replyOut)
        {
            if (!m_sessionActive || request.session != m_session)
            {
                Ack(mark4_OtaOp_ABORT, request.session, mark4_OtaResult_BAD_SESSION, replyOut);
                return;
            }
            const std::uint32_t session = m_session;
            closeSession();
            Ack(mark4_OtaOp_ABORT, session, mark4_OtaResult_OTA_OK, replyOut);
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
        bool m_selfConfirmOnContact = true;  ///< a trial confirms itself on ground contact
        bool m_sessionActive = false;        ///< a transfer session is open
        bool m_sessionFresh = false;         ///< opened, but no timeout sweep ran yet
        std::uint32_t m_session = 0U;        ///< nonce of the open session
        std::uint8_t m_targetSlot = 0U;      ///< slot being written, never the running one
        std::uint32_t m_imageSize = 0U;      ///< image bytes announced at begin
        std::uint32_t m_imageCrc = 0U;       ///< image CRC announced at begin
        std::uint32_t m_nextOffset = 0U;     ///< first image byte still missing
        std::uint32_t m_chunksSinceAck = 0U; ///< in-order chunks since the last ack
        std::uint64_t m_lastPacketUs = 0U;   ///< arrival time of the last session message [us]
    };
} // namespace mark4
