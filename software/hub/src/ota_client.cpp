/// @file
/// @brief Session state machine implementation. Time enters through tick()
///        and messages through onEnvelope(); nothing here reads a clock or a
///        socket, which is what lets the whole flow run against a scripted
///        board in a unit test.

#include "hub/ota_client.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>
#include <utility>

namespace mark4
{
    namespace
    {
        constexpr std::uint64_t US_PER_MS = 1000U;

        /// @return a non-zero session nonce; zero is the wire's "no session"
        std::uint32_t drawSession()
        {
            std::random_device source;
            std::mt19937 generator(source());
            std::uniform_int_distribution<std::uint32_t> spread(1U, UINT32_MAX);
            return spread(generator);
        }

        /// @brief Names one slot the way an operator reads it on a page.
        /// @param slot OTA_SLOT_A, OTA_SLOT_B, or anything else
        /// @return "A", "B", or "?"
        const char *slotLetter(std::uint8_t slot)
        {
            if (slot == OTA_SLOT_A)
            {
                return "A";
            }
            if (slot == OTA_SLOT_B)
            {
                return "B";
            }
            return "?";
        }

        /// @brief Names one build the way an operator reads it in a verdict.
        /// @param epoch build epoch of an image header, 0 when there is none
        /// @return "the build of <UTC time>", or the two headerless cases
        std::string buildText(std::uint32_t epoch)
        {
            if (epoch == 0U)
            {
                return "an image with no header";
            }
            if (epoch == OTA_IMAGE_UNSTAMPED)
            {
                return "an unpackaged build";
            }
            const auto seconds = static_cast<std::time_t>(epoch);
            std::tm utc{};
            static_cast<void>(gmtime_r(&seconds, &utc));
            // "YYYY-mm-dd HH:MM:SS" is 19 characters plus the terminator.
            constexpr std::size_t TEXT_CAPACITY = 20U;
            std::array<char, TEXT_CAPACITY> text{};
            static_cast<void>(std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S", &utc));
            return std::string("the build of ") + text.data() + " UTC";
        }

        /// @brief Builds an envelope holding one body, chosen by tag.
        /// @param tag mark4_Envelope_*_tag of the body
        /// @return zeroed envelope with that body selected
        mark4_Envelope envelopeOf(pb_size_t tag)
        {
            mark4_Envelope envelope = mark4_Envelope_init_zero;
            envelope.which_body = tag;
            return envelope;
        }
    } // namespace

    const char *otaPhaseName(OtaPhase phase)
    {
        switch (phase)
        {
            case OtaPhase::IDLE:
                return "idle";
            case OtaPhase::QUERY:
                return "query";
            case OtaPhase::ERASING:
                return "erasing";
            case OtaPhase::TRANSFER:
                return "transfer";
            case OtaPhase::VERIFYING:
                return "verifying";
            case OtaPhase::REBOOTING:
                return "rebooting";
            case OtaPhase::WAITING_BOARD:
                return "waitingBoard";
            case OtaPhase::TESTING:
                return "testing";
            case OtaPhase::CONFIRMED:
                return "confirmed";
            case OtaPhase::ROLLED_BACK:
                return "rolledBack";
            case OtaPhase::REVERTING:
                return "reverting";
            case OtaPhase::FAILED:
                return "failed";
        }
        return "unknown";
    }

    const char *otaVerdictName(OtaVerdict verdict)
    {
        switch (verdict)
        {
            case OtaVerdict::NONE:
                return "none";
            case OtaVerdict::CONFIRMED:
                return "confirmed";
            case OtaVerdict::ROLLED_BACK:
                return "rolledBack";
            case OtaVerdict::REVERTED:
                return "reverted";
            case OtaVerdict::FAILED:
                return "failed";
        }
        return "unknown";
    }

    const char *otaSlotStateName(mark4_OtaSlotState state)
    {
        switch (state)
        {
            case mark4_OtaSlotState_STAGED:
                return "staged";
            case mark4_OtaSlotState_TESTING:
                return "testing";
            case mark4_OtaSlotState_VALID:
                return "valid";
            case mark4_OtaSlotState_BAD:
                return "bad";
            case mark4_OtaSlotState_EMPTY:
                return "empty";
        }
        return "unknown";
    }

    std::string otaResultText(mark4_OtaResult result)
    {
        switch (result)
        {
            case mark4_OtaResult_OTA_OK:
                return "ok";
            case mark4_OtaResult_DENIED_ARMED:
                return "the board refused: it is armed";
            case mark4_OtaResult_DENIED_VOLTAGE:
                return "the board refused: the battery is under the update floor";
            case mark4_OtaResult_DENIED_BUSY:
                return "the board refused: another update session is already open";
            case mark4_OtaResult_BAD_SESSION:
                return "the board does not know this session any more";
            case mark4_OtaResult_BAD_STATE:
                return "the board refused: the request makes no sense in its current state";
            case mark4_OtaResult_BAD_IMAGE:
                return "the board rejected the image: wrong chip, wrong slot or too large";
            case mark4_OtaResult_CRC_MISMATCH:
                return "the image in flash does not match the announced CRC";
            case mark4_OtaResult_STORE_FAILURE:
                return "the board could not erase, program or record the update";
        }
        return "the board answered with unknown refusal code " +
               std::to_string(static_cast<int>(result));
    }

    OtaClient::OtaClient(Config config)
        : m_config(config)
    {
    }

    void OtaClient::setSink(MessageSink sink)
    {
        m_sink = std::move(sink);
    }

    void OtaClient::setOnChange(ChangeHandler handler)
    {
        m_onChange = std::move(handler);
    }

    void OtaClient::setDefaultBundlePath(std::string path)
    {
        m_defaultBundlePath = std::move(path);
        if (m_bundlePath.empty())
        {
            m_bundlePath = m_defaultBundlePath;
        }
    }

    bool OtaClient::busy() const
    {
        switch (m_phase)
        {
            case OtaPhase::QUERY:
            case OtaPhase::ERASING:
            case OtaPhase::TRANSFER:
            case OtaPhase::VERIFYING:
            case OtaPhase::REBOOTING:
            case OtaPhase::WAITING_BOARD:
            case OtaPhase::TESTING:
            case OtaPhase::REVERTING:
                return true;
            case OtaPhase::IDLE:
            case OtaPhase::CONFIRMED:
            case OtaPhase::ROLLED_BACK:
            case OtaPhase::FAILED:
                return false;
        }
        return false;
    }

    std::string OtaClient::verdictText() const
    {
        const std::string board =
            m_board.seen
                ? buildText(m_board.buildEpoch) +
                      (m_board.gitHash.empty() ? std::string{} : " (" + m_board.gitHash + ")") +
                      " from slot " + slotLetter(m_board.runningSlot)
                : std::string("an unknown firmware");
        switch (m_verdict)
        {
            case OtaVerdict::NONE:
                return {};
            case OtaVerdict::CONFIRMED:
                return "update confirmed: the board runs " + board + ".";
            case OtaVerdict::ROLLED_BACK:
                return "rolled back: the new image did not prove itself, the board runs " + board +
                       " again.";
            case OtaVerdict::REVERTED:
                return "reverted: the board runs " + board + ".";
            case OtaVerdict::FAILED:
                return "update failed: " + m_lastError;
        }
        return {};
    }

    bool OtaClient::start(const std::string &bundlePath, std::uint64_t nowUs, std::string &errorOut)
    {
        if (busy())
        {
            errorOut = "an update is already running";
            return false;
        }
        if (!m_sink)
        {
            errorOut = "no route to the board";
            return false;
        }
        const std::string path = bundlePath.empty() ? m_defaultBundlePath : bundlePath;
        m_bundlePath = path;
        OtaBundle bundle;
        if (!loadOtaBundle(path, bundle, errorOut))
        {
            // A bundle that cannot be read is a finished session, not a
            // refusal to remember: the page must keep showing why.
            m_bundle = OtaBundle{};
            m_verdict = OtaVerdict::FAILED;
            m_lastError = errorOut;
            enter(OtaPhase::FAILED);
            return false;
        }
        m_bundle = std::move(bundle);
        std::error_code ignored;
        m_bundleFileTime = std::filesystem::last_write_time(path, ignored);
        m_bundleFileSize = std::filesystem::file_size(path, ignored);
        m_lastError.clear();
        m_verdict = OtaVerdict::NONE;
        m_progress = OtaProgress{};
        m_targetSlot = OTA_SLOT_COUNT;
        m_session = 0U;
        m_chunkData = 0U;
        m_previousKnown = false;
        m_previousBuildEpoch = 0U;
        m_ackTries = 0U;
        m_statusTries = 1U;
        m_nextStatusUs = nowUs + m_config.statusPeriodMs * US_PER_MS;
        enter(OtaPhase::QUERY);
        if (!sendStatusRequest())
        {
            errorOut = m_lastError;
            return false;
        }
        return true;
    }

    bool OtaClient::abortSession(std::uint64_t nowUs, std::string &errorOut)
    {
        static_cast<void>(nowUs);
        if (!busy())
        {
            errorOut = "no update to abort";
            return false;
        }
        if (m_phase == OtaPhase::ERASING || m_phase == OtaPhase::TRANSFER ||
            m_phase == OtaPhase::VERIFYING)
        {
            // Best effort: telling the board frees its half-written slot now
            // instead of at its own session timeout. A link already gone is
            // not a reason to refuse the operator's abort.
            mark4_Envelope abort = envelopeOf(mark4_Envelope_ota_abort_tag);
            abort.body.ota_abort.session = m_session;
            std::string reason;
            static_cast<void>(m_sink && m_sink(abort, reason));
        }
        m_progress = OtaProgress{};
        m_targetSlot = OTA_SLOT_COUNT;
        m_session = 0U;
        m_verdict = OtaVerdict::NONE;
        m_lastError = "aborted by the operator";
        enter(OtaPhase::IDLE);
        return true;
    }

    bool OtaClient::revert(std::uint64_t nowUs, std::string &errorOut)
    {
        if (busy() && m_phase != OtaPhase::TESTING)
        {
            errorOut = "an update is running: abort it first";
            return false;
        }
        if (!m_sink)
        {
            errorOut = "no route to the board";
            return false;
        }
        m_lastError.clear();
        m_verdict = OtaVerdict::NONE;
        m_ackTries = 1U;
        m_deadlineUs = nowUs + m_config.ackTimeoutMs * US_PER_MS;
        enter(OtaPhase::REVERTING);
        if (!sendRevert())
        {
            errorOut = m_lastError;
            return false;
        }
        return true;
    }

    bool OtaClient::requestBoardStatus(std::uint64_t nowUs, std::string &errorOut)
    {
        static_cast<void>(nowUs);
        if (!m_sink)
        {
            errorOut = "no route to the board";
            return false;
        }
        std::string reason;
        if (!m_sink(envelopeOf(mark4_Envelope_ota_status_request_tag), reason))
        {
            // A status request is a question, never a session: a board that
            // is not there must not fail an update that is not running.
            errorOut = reason.empty() ? "the board is not reachable" : reason;
            return false;
        }
        return true;
    }

    bool OtaClient::onEnvelope(const mark4_Envelope &envelope, std::uint64_t nowUs)
    {
        switch (envelope.which_body)
        {
            case mark4_Envelope_ota_status_tag:
                onStatus(envelope.body.ota_status, nowUs);
                return true;
            case mark4_Envelope_ota_ack_tag:
                onAck(envelope.body.ota_ack, nowUs);
                return true;
            case mark4_Envelope_ota_chunk_ack_tag:
                onChunkAck(envelope.body.ota_chunk_ack, nowUs);
                return true;
            default:
                return false;
        }
    }

    void OtaClient::refreshBundle(std::uint64_t nowUs)
    {
        if (busy() || nowUs < m_nextBundleCheckUs)
        {
            return;
        }
        m_nextBundleCheckUs = nowUs + BUNDLE_CHECK_MS * US_PER_MS;
        const std::string &path = m_bundlePath.empty() ? m_defaultBundlePath : m_bundlePath;
        if (path.empty())
        {
            return;
        }
        std::error_code failure;
        const auto written = std::filesystem::last_write_time(path, failure);
        const auto size = failure ? 0U : std::filesystem::file_size(path, failure);
        if (failure)
        {
            // Not built yet, or deleted: showing a bundle that no start
            // could send any more would be a lie.
            if (m_bundle.loaded())
            {
                m_bundle = OtaBundle{};
                notifyChange();
            }
            return;
        }
        if (m_bundle.loaded() && written == m_bundleFileTime && size == m_bundleFileSize)
        {
            return;
        }
        // The file is new or changed since the load: a fresh build. A load
        // that fails keeps the old view and retries on the next check; the
        // build may still have been writing the file.
        OtaBundle bundle;
        std::string error;
        if (!loadOtaBundle(path, bundle, error))
        {
            return;
        }
        m_bundle = std::move(bundle);
        m_bundleFileTime = written;
        m_bundleFileSize = size;
        notifyChange();
    }

    void OtaClient::tick(std::uint64_t nowUs)
    {
        refreshBundle(nowUs);
        switch (m_phase)
        {
            case OtaPhase::QUERY:
                if (nowUs >= m_nextStatusUs)
                {
                    if (m_statusTries >= m_config.statusTries)
                    {
                        fail("the board did not answer a status request");
                        return;
                    }
                    ++m_statusTries;
                    m_nextStatusUs = nowUs + m_config.statusPeriodMs * US_PER_MS;
                    static_cast<void>(sendStatusRequest());
                }
                break;
            case OtaPhase::ERASING:
                if (nowUs >= m_deadlineUs)
                {
                    fail(std::string("the board did not finish erasing slot ") +
                         slotLetter(m_targetSlot) + " in time");
                }
                break;
            case OtaPhase::TRANSFER:
                if (nowUs >= m_chunkDeadlineUs)
                {
                    if (m_stalledRounds >= m_config.maxRetries)
                    {
                        fail("the board stopped acknowledging chunks at " +
                             std::to_string(m_progress.ackedBytes) + " of " +
                             std::to_string(m_progress.totalBytes) + " bytes");
                        return;
                    }
                    // Go-back-N: everything above the last cumulative
                    // acknowledgement is unknown, so it all goes again.
                    ++m_progress.retries;
                    ++m_stalledRounds;
                    m_progress.sentBytes = m_progress.ackedBytes;
                    m_nextChunkUs = nowUs;
                    m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;
                    notifyChange();
                }
                pumpChunks(nowUs);
                break;
            case OtaPhase::VERIFYING:
                if (nowUs >= m_deadlineUs)
                {
                    fail("the board did not answer the end of the transfer");
                }
                break;
            case OtaPhase::REBOOTING:
                if (nowUs >= m_settleUntilUs)
                {
                    m_nextStatusUs = nowUs;
                    enter(OtaPhase::WAITING_BOARD);
                }
                break;
            case OtaPhase::WAITING_BOARD:
                if (nowUs >= m_deadlineUs)
                {
                    fail("the board did not come back after the reboot");
                    return;
                }
                if (nowUs >= m_nextStatusUs)
                {
                    m_nextStatusUs = nowUs + m_config.statusPeriodMs * US_PER_MS;
                    static_cast<void>(sendStatusRequest());
                }
                break;
            case OtaPhase::TESTING:
                // The trial image vouches for itself on the first request it
                // serves, so the hub only keeps asking until the metadata
                // says VALID, or the bootloader brings the old image back.
                if (nowUs >= m_nextStatusUs)
                {
                    m_nextStatusUs = nowUs + m_config.statusPeriodMs * US_PER_MS;
                    static_cast<void>(sendStatusRequest());
                }
                break;
            case OtaPhase::REVERTING:
                if (nowUs >= m_deadlineUs)
                {
                    if (m_ackTries >= m_config.maxAckTries)
                    {
                        fail("the board did not acknowledge the revert");
                        return;
                    }
                    ++m_ackTries;
                    m_deadlineUs = nowUs + m_config.ackTimeoutMs * US_PER_MS;
                    static_cast<void>(sendRevert());
                }
                break;
            case OtaPhase::IDLE:
            case OtaPhase::CONFIRMED:
            case OtaPhase::ROLLED_BACK:
            case OtaPhase::FAILED:
                break;
        }
    }

    bool OtaClient::emit(const mark4_Envelope &envelope)
    {
        if (!m_sink)
        {
            fail("no route to the board");
            return false;
        }
        std::string reason;
        if (!m_sink(envelope, reason))
        {
            fail(reason.empty() ? std::string("the board link went away") : reason);
            return false;
        }
        return true;
    }

    bool OtaClient::sendStatusRequest()
    {
        return emit(envelopeOf(mark4_Envelope_ota_status_request_tag));
    }

    bool OtaClient::sendReboot()
    {
        return emit(envelopeOf(mark4_Envelope_reboot_tag));
    }

    bool OtaClient::sendRevert()
    {
        return emit(envelopeOf(mark4_Envelope_ota_revert_tag));
    }

    bool OtaClient::sendChunk()
    {
        const OtaBundleImage *image = findOtaBundleImage(m_bundle, m_targetSlot);
        if (image == nullptr)
        {
            fail("the bundle lost the image it was sending");
            return false;
        }
        const std::uint32_t remaining = m_progress.totalBytes - m_progress.sentBytes;
        const std::uint32_t length = std::min(remaining, m_chunkData);
        mark4_Envelope envelope = envelopeOf(mark4_Envelope_ota_chunk_tag);
        mark4_OtaChunk &chunk = envelope.body.ota_chunk;
        chunk.session = m_session;
        chunk.offset = m_progress.sentBytes;
        chunk.data.size = static_cast<pb_size_t>(length);
        std::memcpy(chunk.data.bytes, image->bytes.data() + m_progress.sentBytes, length);
        if (!emit(envelope))
        {
            return false;
        }
        m_progress.sentBytes += length;
        return true;
    }

    void OtaClient::pumpChunks(std::uint64_t nowUs)
    {
        if (m_phase != OtaPhase::TRANSFER || m_chunkData == 0U)
        {
            return;
        }
        const std::uint32_t windowBytes = OTA_CHUNK_ACK_WINDOW * m_chunkData;
        bool sent = false;
        // One chunk per pass at most, then the pacing delay: the loop only
        // repeats when the delay is zero, which is how a test drains a whole
        // window in one tick. The window itself is the real flow control -
        // this delay only keeps a WiFi burst from outrunning the relay's UART.
        while (m_progress.sentBytes < m_progress.totalBytes &&
               m_progress.sentBytes - m_progress.ackedBytes < windowBytes && nowUs >= m_nextChunkUs)
        {
            if (!sendChunk())
            {
                return;
            }
            sent = true;
            m_nextChunkUs = nowUs + m_config.chunkDelayUs;
        }
        if (sent)
        {
            m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;
        }
    }

    void OtaClient::openTransfer(std::uint64_t nowUs)
    {
        if (static_cast<std::uint8_t>(m_board.mcu) != m_bundle.mcuId)
        {
            fail("the bundle is built for mcu " + std::to_string(m_bundle.mcuId) +
                 " and the board is mcu " + std::to_string(static_cast<int>(m_board.mcu)));
            return;
        }
        if (m_board.updaterBusy)
        {
            fail("the board already has an update session open: reboot it and retry");
            return;
        }
        if (m_board.runningSlot >= OTA_SLOT_COUNT)
        {
            fail("the board reports running from a slot this system does not have");
            return;
        }
        // The running slot is never written: the target is the other one, and
        // that is the whole safety property of the dual-bank layout.
        m_targetSlot = static_cast<std::uint8_t>(1U - m_board.runningSlot);
        const OtaBundleImage *image = findOtaBundleImage(m_bundle, m_targetSlot);
        if (image == nullptr)
        {
            fail(std::string("the bundle holds no image for slot ") + slotLetter(m_targetSlot));
            return;
        }
        if (m_board.slotSize != 0U && image->size > m_board.slotSize)
        {
            fail("the image is " + std::to_string(image->size) + " bytes and a slot holds " +
                 std::to_string(m_board.slotSize));
            return;
        }
        m_chunkData =
            m_board.maxChunkData == 0U
                ? static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE)
                : std::min(static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE), m_board.maxChunkData);
        if (m_chunkData == 0U)
        {
            fail("the board accepts no chunk data at all");
            return;
        }

        // The identity the board runs right now is what a rollback will look
        // like when it comes back, so it is captured before anything changes.
        m_previousKnown = true;
        m_previousBuildEpoch = m_board.buildEpoch;

        m_session = drawSession();
        m_progress = OtaProgress{};
        m_progress.totalBytes = image->size;
        m_stalledRounds = 0U;
        m_resendOffset = 0U;
        m_resendGuardUs = 0U;
        mark4_Envelope begin = envelopeOf(mark4_Envelope_ota_begin_tag);
        begin.body.ota_begin.session = m_session;
        begin.body.ota_begin.image_size = image->size;
        begin.body.ota_begin.image_crc = image->crc32;
        m_deadlineUs = nowUs + m_config.beginTimeoutMs * US_PER_MS;
        enter(OtaPhase::ERASING);
        static_cast<void>(emit(begin));
    }

    void OtaClient::onStatus(const mark4_OtaStatus &status, std::uint64_t nowUs)
    {
        if ((m_phase == OtaPhase::REBOOTING || m_phase == OtaPhase::WAITING_BOARD) &&
            nowUs < m_settleUntilUs)
        {
            // The old image can still answer one request between the reboot
            // command and the reset itself; that answer is not a verdict.
            return;
        }

        m_board.seen = true;
        m_board.seenAtUs = nowUs;
        m_board.mcu = status.mcu;
        m_board.runningSlot = static_cast<std::uint8_t>(status.running_slot);
        m_board.activeSlot = static_cast<std::uint8_t>(status.active_slot);
        for (std::size_t slot = 0U; slot < OTA_SLOT_COUNT; ++slot)
        {
            m_board.slots[slot].state = status.slots[slot].state;
            m_board.slots[slot].buildEpoch = status.slots[slot].build_epoch;
            m_board.slots[slot].gitHash = status.slots[slot].git_hash;
        }
        m_board.updaterBusy = status.updater_busy;
        // The running slot's identity doubles as the board's, which is what
        // every verdict compares.
        const OtaSlotInfo &running =
            m_board.slots[m_board.runningSlot < OTA_SLOT_COUNT ? m_board.runningSlot : 0U];
        m_board.buildEpoch = running.buildEpoch;
        m_board.gitHash = running.gitHash;
        m_board.slotSize = status.slot_size;
        m_board.maxChunkData = status.max_chunk_data;

        switch (m_phase)
        {
            case OtaPhase::QUERY:
                openTransfer(nowUs);
                return;
            case OtaPhase::WAITING_BOARD:
                judgeReturn(nowUs);
                return;
            case OtaPhase::TESTING:
                if (boardRunsBundle())
                {
                    const mark4_OtaSlotState state = m_board.runningSlot < OTA_SLOT_COUNT
                                                         ? m_board.slots[m_board.runningSlot].state
                                                         : mark4_OtaSlotState_EMPTY;
                    if (state != mark4_OtaSlotState_TESTING)
                    {
                        // The image confirmed itself since the last answer.
                        m_verdict = OtaVerdict::CONFIRMED;
                        enter(OtaPhase::CONFIRMED);
                        return;
                    }
                }
                else if (boardRunsPrevious())
                {
                    // The trial image was booted and did not survive: the
                    // bootloader has already brought the old one back.
                    m_verdict = OtaVerdict::ROLLED_BACK;
                    enter(OtaPhase::ROLLED_BACK);
                    return;
                }
                break;
            // Every other phase only takes the snapshot above: a status
            // message in the middle of an erase or a transfer is information,
            // not an event.
            case OtaPhase::REVERTING:
            case OtaPhase::IDLE:
            case OtaPhase::ERASING:
            case OtaPhase::TRANSFER:
            case OtaPhase::VERIFYING:
            case OtaPhase::REBOOTING:
            case OtaPhase::CONFIRMED:
            case OtaPhase::ROLLED_BACK:
            case OtaPhase::FAILED:
                break;
        }
        notifyChange();
    }

    void OtaClient::judgeReturn(std::uint64_t nowUs)
    {
        if (boardRunsBundle())
        {
            const mark4_OtaSlotState state = m_board.runningSlot < OTA_SLOT_COUNT
                                                 ? m_board.slots[m_board.runningSlot].state
                                                 : mark4_OtaSlotState_EMPTY;
            if (state == mark4_OtaSlotState_TESTING)
            {
                m_nextStatusUs = nowUs + m_config.statusPeriodMs * US_PER_MS;
                enter(OtaPhase::TESTING);
                return;
            }
            // Nothing left on trial: the slot is already trusted, so the
            // update stands with no confirmation to send.
            m_verdict = OtaVerdict::CONFIRMED;
            enter(OtaPhase::CONFIRMED);
            return;
        }
        if (boardRunsPrevious())
        {
            m_verdict = OtaVerdict::ROLLED_BACK;
            enter(OtaPhase::ROLLED_BACK);
            return;
        }
        fail("the board came back running " + buildText(m_board.buildEpoch) +
             (m_board.gitHash.empty() ? std::string{} : " (" + m_board.gitHash + ")") +
             ", which is neither the bundle nor what it ran before");
    }

    void OtaClient::onAck(const mark4_OtaAck &ack, std::uint64_t nowUs)
    {
        switch (ack.op)
        {
            case mark4_OtaOp_BEGIN:
                if (m_phase != OtaPhase::ERASING || ack.session != m_session)
                {
                    return;
                }
                if (ack.result != mark4_OtaResult_OTA_OK)
                {
                    fail(otaResultText(ack.result));
                    return;
                }
                m_progress.sentBytes = 0U;
                m_progress.ackedBytes = 0U;
                m_progress.retries = 0U;
                m_nextChunkUs = nowUs;
                m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;
                enter(OtaPhase::TRANSFER);
                pumpChunks(nowUs);
                return;
            case mark4_OtaOp_FINISH:
                if (m_phase != OtaPhase::VERIFYING || ack.session != m_session)
                {
                    return;
                }
                if (ack.result != mark4_OtaResult_OTA_OK)
                {
                    fail(otaResultText(ack.result));
                    return;
                }
                // Staged. The trial boot is the existing reboot command:
                // nothing in the wire had to move for the update path.
                m_settleUntilUs = nowUs + m_config.rebootSettleMs * US_PER_MS;
                m_deadlineUs = nowUs + m_config.boardReturnTimeoutMs * US_PER_MS;
                enter(OtaPhase::REBOOTING);
                static_cast<void>(sendReboot());
                return;
            case mark4_OtaOp_REVERT:
                if (m_phase != OtaPhase::REVERTING)
                {
                    return;
                }
                if (ack.result != mark4_OtaResult_OTA_OK)
                {
                    fail(otaResultText(ack.result));
                    return;
                }
                m_verdict = OtaVerdict::REVERTED;
                m_settleUntilUs = nowUs + m_config.rebootSettleMs * US_PER_MS;
                m_deadlineUs = nowUs + m_config.boardReturnTimeoutMs * US_PER_MS;
                // The revert only flips the active slot; the reboot is what
                // makes it the running one, exactly as after a transfer.
                static_cast<void>(sendReboot());
                enter(OtaPhase::IDLE);
                return;
            case mark4_OtaOp_CHUNK:
            case mark4_OtaOp_ABORT:
            case mark4_OtaOp_OTA_OP_UNSPECIFIED:
                return;
        }
    }

    void OtaClient::onChunkAck(const mark4_OtaChunkAck &ack, std::uint64_t nowUs)
    {
        const std::uint32_t nextOffset = ack.next_offset;
        if (ack.session != m_session || m_session == 0U)
        {
            return;
        }
        if (m_phase != OtaPhase::TRANSFER && m_phase != OtaPhase::VERIFYING)
        {
            return;
        }
        if (nextOffset > m_progress.totalBytes)
        {
            fail("the board acknowledged more bytes than the image holds");
            return;
        }
        if (nextOffset < m_progress.ackedBytes)
        {
            if (m_phase != OtaPhase::VERIFYING)
            {
                // A late duplicate of an older window; mid transfer the
                // cumulative offset never goes backwards.
                return;
            }
            // In answer to OtaFinish the board's offset is the authority:
            // bytes the sender believed written are not there, so the
            // transfer resumes from where the board says it stopped.
            m_progress.ackedBytes = nextOffset;
            m_progress.sentBytes = nextOffset;
            m_nextChunkUs = nowUs;
            m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;
            enter(OtaPhase::TRANSFER);
            pumpChunks(nowUs);
            return;
        }
        if (nextOffset == m_progress.ackedBytes && m_progress.ackedBytes != m_progress.sentBytes)
        {
            // A repeated offset is the board saying it dropped an
            // out-of-order chunk: resend from there without waiting out the
            // silence timeout. One lost chunk makes every later chunk of
            // the window repeat the same offset, so a resend answers the
            // whole burst once and the echoes of that window are ignored -
            // counting each echo as a refusal turned a single radio loss
            // into an instant failure on the bench.
            if (nextOffset == m_resendOffset && nowUs < m_resendGuardUs)
            {
                return;
            }
            if (m_stalledRounds >= m_config.maxRetries)
            {
                fail("the board kept refusing chunks at " + std::to_string(m_progress.ackedBytes) +
                     " of " + std::to_string(m_progress.totalBytes) + " bytes");
                return;
            }
            ++m_progress.retries;
            ++m_stalledRounds;
            m_resendOffset = nextOffset;
            m_resendGuardUs = nowUs + (m_config.chunkAckTimeoutMs * US_PER_MS);
            m_progress.sentBytes = m_progress.ackedBytes;
            m_nextChunkUs = nowUs;
            m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;
            notifyChange();
            pumpChunks(nowUs);
            return;
        }

        // Forward progress: only a genuine stall may fail the transfer, so
        // the round counter starts over (retries stays, it is the tally the
        // page shows).
        m_stalledRounds = 0U;
        m_progress.ackedBytes = nextOffset;
        m_progress.sentBytes = std::max(m_progress.sentBytes, nextOffset);
        m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;

        if (m_progress.ackedBytes >= m_progress.totalBytes)
        {
            mark4_Envelope finish = envelopeOf(mark4_Envelope_ota_finish_tag);
            finish.body.ota_finish.session = m_session;
            m_deadlineUs = nowUs + m_config.finishTimeoutMs * US_PER_MS;
            enter(OtaPhase::VERIFYING);
            static_cast<void>(emit(finish));
            return;
        }
        if (m_phase == OtaPhase::VERIFYING)
        {
            // A chunk acknowledgement in answer to OtaFinish means bytes
            // are still missing: the transfer resumes from that offset.
            m_nextChunkUs = nowUs;
            enter(OtaPhase::TRANSFER);
        }
        else
        {
            notifyChange();
        }
        pumpChunks(nowUs);
    }

    bool OtaClient::boardRunsBundle() const
    {
        if (!m_board.seen)
        {
            return false;
        }
        // The build epoch IS the identity: unlike the git hash it tells two
        // packagings of the same dirty tree apart. The hash stays display
        // only. Two headerless or two unpackaged images compare equal here,
        // which is the best either side can claim about itself.
        return m_board.buildEpoch == m_bundle.buildEpoch;
    }

    bool OtaClient::boardRunsPrevious() const
    {
        if (!m_board.seen || !m_previousKnown)
        {
            return false;
        }
        return m_board.buildEpoch == m_previousBuildEpoch;
    }

    void OtaClient::fail(const std::string &reason)
    {
        m_lastError = reason;
        m_verdict = OtaVerdict::FAILED;
        enter(OtaPhase::FAILED);
    }

    void OtaClient::enter(OtaPhase phase)
    {
        m_phase = phase;
        notifyChange();
    }

    void OtaClient::notifyChange()
    {
        if (m_onChange)
        {
            m_onChange();
        }
    }
} // namespace mark4
