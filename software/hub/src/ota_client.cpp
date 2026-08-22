/// @file
/// @brief Session state machine implementation. Time enters through tick()
///        and packets through onPacket(); nothing here reads a clock or a
///        socket, which is what lets the whole flow run against a scripted
///        board in a unit test.

#include "hub/ota_client.hpp"

#include <algorithm>
#include <cstring>
#include <random>
#include <utility>

#include "hub/json_codec.hpp"
#include "hub/packed_field.hpp"
#include "protocol/commands.hpp"
#include "protocol/header.hpp"

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

    const char *otaSlotStateName(std::uint8_t state)
    {
        switch (state)
        {
            case OTA_SLOT_STAGED:
                return "staged";
            case OTA_SLOT_TESTING:
                return "testing";
            case OTA_SLOT_VALID:
                return "valid";
            case OTA_SLOT_BAD:
                return "bad";
            case OTA_SLOT_EMPTY:
                return "empty";
            default:
                return "unknown";
        }
    }

    std::string otaResultText(std::uint8_t result)
    {
        switch (result)
        {
            case OTA_RESULT_OK:
                return "ok";
            case OTA_RESULT_DENIED_ARMED:
                return "the board refused: it is armed";
            case OTA_RESULT_DENIED_VOLTAGE:
                return "the board refused: the battery is under the update floor";
            case OTA_RESULT_DENIED_BUSY:
                return "the board refused: another update session is already open";
            case OTA_RESULT_BAD_SESSION:
                return "the board does not know this session any more";
            case OTA_RESULT_BAD_STATE:
                return "the board refused: the request makes no sense in its current state";
            case OTA_RESULT_BAD_IMAGE:
                return "the board rejected the image: wrong chip, wrong slot or too large";
            case OTA_RESULT_CRC_MISMATCH:
                return "the image in flash does not match the announced CRC";
            case OTA_RESULT_STORE_FAILURE:
                return "the board could not erase, program or record the update";
            default:
                return "the board answered with unknown refusal code " + std::to_string(result);
        }
    }

    OtaClient::OtaClient(Config config)
        : m_config(config)
    {
    }

    void OtaClient::setSink(PacketSink sink)
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

    void OtaClient::setAutoConfirm(bool on)
    {
        if (m_config.autoConfirm == on)
        {
            return;
        }
        m_config.autoConfirm = on;
        notifyChange();
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

    bool OtaClient::confirmReady() const
    {
        return m_phase == OtaPhase::TESTING && !m_confirmSent && m_healthySinceUs != 0U &&
               m_healthyCount >= m_config.healthyStatuses;
    }

    std::string OtaClient::verdictText() const
    {
        const std::string board =
            m_board.seen
                ? "v" +
                      otaVersionText(
                          m_board.versionMajor, m_board.versionMinor, m_board.versionPatch) +
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
        m_lastError.clear();
        m_verdict = OtaVerdict::NONE;
        m_progress = OtaProgress{};
        m_targetSlot = OTA_SLOT_COUNT;
        m_session = 0U;
        m_chunkData = 0U;
        m_confirmSent = false;
        m_previousKnown = false;
        m_previousGitHash.clear();
        m_healthyCount = 0U;
        m_healthySinceUs = 0U;
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
            OtaAbortPacket packet{};
            packet.version = PROTOCOL_VERSION;
            packet.type = static_cast<std::uint8_t>(PacketType::OTA_ABORT);
            packet.session = m_session;
            const auto bytes = wireBytes(packet);
            std::string reason;
            static_cast<void>(m_sink && m_sink(bytes.data(), bytes.size(), reason));
        }
        m_progress = OtaProgress{};
        m_targetSlot = OTA_SLOT_COUNT;
        m_session = 0U;
        m_confirmSent = false;
        m_verdict = OtaVerdict::NONE;
        m_lastError = "aborted by the operator";
        enter(OtaPhase::IDLE);
        return true;
    }

    bool OtaClient::confirm(std::uint64_t nowUs, std::string &errorOut)
    {
        if (m_phase != OtaPhase::TESTING)
        {
            errorOut = "the board is not running a trial image";
            return false;
        }
        if (m_confirmSent)
        {
            errorOut = "a confirmation is already on its way";
            return false;
        }
        sendConfirm(nowUs);
        if (m_phase == OtaPhase::FAILED)
        {
            errorOut = m_lastError;
            return false;
        }
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
        OtaRevertPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_REVERT);
        const auto bytes = wireBytes(packet);
        m_lastError.clear();
        m_verdict = OtaVerdict::NONE;
        m_confirmSent = false;
        m_ackTries = 1U;
        m_deadlineUs = nowUs + m_config.ackTimeoutMs * US_PER_MS;
        enter(OtaPhase::REVERTING);
        if (!emit(bytes.data(), bytes.size()))
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
        OtaStatusRequestPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_STATUS_REQUEST);
        const auto bytes = wireBytes(packet);
        std::string reason;
        if (!m_sink(bytes.data(), bytes.size(), reason))
        {
            // A status request is a question, never a session: a board that
            // is not there must not fail an update that is not running.
            errorOut = reason.empty() ? "the board is not reachable" : reason;
            return false;
        }
        return true;
    }

    bool OtaClient::onPacket(const std::uint8_t *data, std::size_t size, std::uint64_t nowUs)
    {
        if (size == OTA_STATUS_PACKET_SIZE && hasHeader(data, size, PacketType::OTA_STATUS))
        {
            onStatus(data, nowUs);
            return true;
        }
        if (size == OTA_ACK_PACKET_SIZE && hasHeader(data, size, PacketType::OTA_ACK))
        {
            onAck(data, nowUs);
            return true;
        }
        if (size == OTA_CHUNK_ACK_PACKET_SIZE && hasHeader(data, size, PacketType::OTA_CHUNK_ACK))
        {
            onChunkAck(data, nowUs);
            return true;
        }
        return false;
    }

    void OtaClient::tick(std::uint64_t nowUs)
    {
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
                    if (m_progress.retries >= m_config.maxRetries)
                    {
                        fail("the board stopped acknowledging chunks at " +
                             std::to_string(m_progress.ackedBytes) + " of " +
                             std::to_string(m_progress.totalBytes) + " bytes");
                        return;
                    }
                    // Go-back-N: everything above the last cumulative
                    // acknowledgement is unknown, so it all goes again.
                    ++m_progress.retries;
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
                if (m_confirmSent)
                {
                    if (nowUs >= m_deadlineUs)
                    {
                        if (m_ackTries >= m_config.maxAckTries)
                        {
                            fail("the board did not acknowledge the confirmation");
                            return;
                        }
                        m_confirmSent = false;
                        sendConfirm(nowUs);
                    }
                    break;
                }
                // The trial image must keep answering: the healthy link IS
                // the property being tested, so the polling continues.
                if (nowUs >= m_nextStatusUs)
                {
                    m_nextStatusUs = nowUs + m_config.statusPeriodMs * US_PER_MS;
                    static_cast<void>(sendStatusRequest());
                }
                if (m_config.autoConfirm && m_healthySinceUs != 0U &&
                    m_healthyCount >= m_config.healthyStatuses &&
                    nowUs - m_healthySinceUs >= m_config.healthyLinkMs * US_PER_MS)
                {
                    sendConfirm(nowUs);
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
                    OtaRevertPacket packet{};
                    packet.version = PROTOCOL_VERSION;
                    packet.type = static_cast<std::uint8_t>(PacketType::OTA_REVERT);
                    const auto bytes = wireBytes(packet);
                    static_cast<void>(emit(bytes.data(), bytes.size()));
                }
                break;
            case OtaPhase::IDLE:
            case OtaPhase::CONFIRMED:
            case OtaPhase::ROLLED_BACK:
            case OtaPhase::FAILED:
                break;
        }
    }

    bool OtaClient::emit(const std::uint8_t *data, std::size_t size)
    {
        if (!m_sink)
        {
            fail("no route to the board");
            return false;
        }
        std::string reason;
        if (!m_sink(data, size, reason))
        {
            fail(reason.empty() ? std::string("the board link went away") : reason);
            return false;
        }
        return true;
    }

    bool OtaClient::sendStatusRequest()
    {
        OtaStatusRequestPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_STATUS_REQUEST);
        const auto bytes = wireBytes(packet);
        return emit(bytes.data(), bytes.size());
    }

    bool OtaClient::sendReboot()
    {
        RebootCommandPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::REBOOT_COMMAND);
        packet.magic = BOARD_REBOOT_MAGIC;
        const auto bytes = wireBytes(packet);
        return emit(bytes.data(), bytes.size());
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
        OtaChunkPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_CHUNK);
        packet.session = m_session;
        packet.offset = m_progress.sentBytes;
        packet.length = static_cast<std::uint8_t>(length);
        // The data field is copied in as a whole aligned value: a packed
        // member is written through, never bound to.
        std::array<std::uint8_t, OTA_CHUNK_DATA_SIZE> data{};
        std::memcpy(data.data(), image->bytes.data() + m_progress.sentBytes, length);
        writePackedField(&packet.data, data);
        const auto bytes = wireBytes(packet);
        if (!emit(bytes.data(), bytes.size()))
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
        // this delay only keeps a WiFi burst from outrunning the bridge UART.
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
        if (m_board.mcuId != m_bundle.mcuId)
        {
            fail("the bundle is built for mcu " + std::to_string(m_bundle.mcuId) +
                 " and the board is mcu " + std::to_string(m_board.mcuId));
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
        m_chunkData = m_board.maxChunkData == 0U
                          ? static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE)
                          : std::min(static_cast<std::uint32_t>(OTA_CHUNK_DATA_SIZE),
                                     static_cast<std::uint32_t>(m_board.maxChunkData));
        if (m_chunkData == 0U)
        {
            fail("the board accepts no chunk data at all");
            return;
        }

        // The identity the board runs right now is what a rollback will look
        // like when it comes back, so it is captured before anything changes.
        m_previousKnown = true;
        m_previousMajor = m_board.versionMajor;
        m_previousMinor = m_board.versionMinor;
        m_previousPatch = m_board.versionPatch;
        m_previousGitHash = m_board.gitHash;

        m_session = drawSession();
        m_progress = OtaProgress{};
        m_progress.totalBytes = image->size;
        OtaBeginPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_BEGIN);
        packet.session = m_session;
        packet.imageSize = image->size;
        packet.imageCrc = image->crc32;
        const auto bytes = wireBytes(packet);
        m_deadlineUs = nowUs + m_config.beginTimeoutMs * US_PER_MS;
        enter(OtaPhase::ERASING);
        static_cast<void>(emit(bytes.data(), bytes.size()));
    }

    void OtaClient::onStatus(const std::uint8_t *data, std::uint64_t nowUs)
    {
        OtaStatusPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));

        if ((m_phase == OtaPhase::REBOOTING || m_phase == OtaPhase::WAITING_BOARD) &&
            nowUs < m_settleUntilUs)
        {
            // The old image can still answer one request between the reboot
            // command and the reset itself; that answer is not a verdict.
            return;
        }

        m_board.seen = true;
        m_board.seenAtUs = nowUs;
        m_board.mcuId = packet.mcuId;
        m_board.runningSlot = packet.runningSlot;
        m_board.activeSlot = packet.activeSlot;
        m_board.slotState = readPackedField(&packet.slotState);
        m_board.updaterBusy = packet.updaterBusy != 0U;
        m_board.versionMajor = packet.versionMajor;
        m_board.versionMinor = packet.versionMinor;
        m_board.versionPatch = packet.versionPatch;
        m_board.gitHash = otaGitHashText(readPackedField(&packet.gitHash));
        m_board.slotSize = packet.slotSize;
        m_board.maxChunkData = packet.maxChunkData;

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
                    if (m_healthySinceUs == 0U)
                    {
                        m_healthySinceUs = nowUs;
                    }
                    ++m_healthyCount;
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
            // packet in the middle of an erase or a transfer is information,
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
            const std::uint8_t state = m_board.runningSlot < OTA_SLOT_COUNT
                                           ? m_board.slotState[m_board.runningSlot]
                                           : static_cast<std::uint8_t>(OTA_SLOT_EMPTY);
            if (state == OTA_SLOT_TESTING)
            {
                m_healthySinceUs = nowUs;
                m_healthyCount = 1U;
                m_confirmSent = false;
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
        fail("the board came back running v" +
             otaVersionText(m_board.versionMajor, m_board.versionMinor, m_board.versionPatch) +
             (m_board.gitHash.empty() ? std::string{} : " (" + m_board.gitHash + ")") +
             ", which is neither the bundle nor what it ran before");
    }

    void OtaClient::sendConfirm(std::uint64_t nowUs)
    {
        OtaConfirmPacket packet{};
        packet.version = PROTOCOL_VERSION;
        packet.type = static_cast<std::uint8_t>(PacketType::OTA_CONFIRM);
        const auto bytes = wireBytes(packet);
        m_confirmSent = true;
        ++m_ackTries;
        m_deadlineUs = nowUs + m_config.ackTimeoutMs * US_PER_MS;
        notifyChange();
        static_cast<void>(emit(bytes.data(), bytes.size()));
    }

    void OtaClient::onAck(const std::uint8_t *data, std::uint64_t nowUs)
    {
        OtaAckPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        const std::uint32_t session = packet.session;
        const auto acked = static_cast<PacketType>(packet.ackedType);

        switch (acked)
        {
            case PacketType::OTA_BEGIN:
                if (m_phase != OtaPhase::ERASING || session != m_session)
                {
                    return;
                }
                if (packet.result != OTA_RESULT_OK)
                {
                    fail(otaResultText(packet.result));
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
            case PacketType::OTA_FINISH:
                if (m_phase != OtaPhase::VERIFYING || session != m_session)
                {
                    return;
                }
                if (packet.result != OTA_RESULT_OK)
                {
                    fail(otaResultText(packet.result));
                    return;
                }
                // Staged. The trial boot is the existing reboot command:
                // nothing in the protocol had to move for the update path.
                m_settleUntilUs = nowUs + m_config.rebootSettleMs * US_PER_MS;
                m_deadlineUs = nowUs + m_config.boardReturnTimeoutMs * US_PER_MS;
                enter(OtaPhase::REBOOTING);
                static_cast<void>(sendReboot());
                return;
            case PacketType::OTA_CONFIRM:
                if (m_phase != OtaPhase::TESTING || !m_confirmSent)
                {
                    return;
                }
                if (packet.result != OTA_RESULT_OK)
                {
                    fail(otaResultText(packet.result));
                    return;
                }
                m_confirmSent = false;
                m_verdict = OtaVerdict::CONFIRMED;
                enter(OtaPhase::CONFIRMED);
                return;
            case PacketType::OTA_REVERT:
                if (m_phase != OtaPhase::REVERTING)
                {
                    return;
                }
                if (packet.result != OTA_RESULT_OK)
                {
                    fail(otaResultText(packet.result));
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
            default:
                return;
        }
    }

    void OtaClient::onChunkAck(const std::uint8_t *data, std::uint64_t nowUs)
    {
        OtaChunkAckPacket packet{};
        std::memcpy(&packet, data, sizeof(packet));
        const std::uint32_t session = packet.session;
        const std::uint32_t nextOffset = packet.nextOffset;
        if (session != m_session || m_session == 0U)
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
            // In answer to OTA_FINISH the board's offset is the authority:
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
            // silence timeout.
            if (m_progress.retries >= m_config.maxRetries)
            {
                fail("the board kept refusing chunks at " + std::to_string(m_progress.ackedBytes) +
                     " of " + std::to_string(m_progress.totalBytes) + " bytes");
                return;
            }
            ++m_progress.retries;
            m_progress.sentBytes = m_progress.ackedBytes;
            m_nextChunkUs = nowUs;
            m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;
            notifyChange();
            pumpChunks(nowUs);
            return;
        }

        m_progress.ackedBytes = nextOffset;
        m_progress.sentBytes = std::max(m_progress.sentBytes, nextOffset);
        m_chunkDeadlineUs = nowUs + m_config.chunkAckTimeoutMs * US_PER_MS;

        if (m_progress.ackedBytes >= m_progress.totalBytes)
        {
            OtaFinishPacket finish{};
            finish.version = PROTOCOL_VERSION;
            finish.type = static_cast<std::uint8_t>(PacketType::OTA_FINISH);
            finish.session = m_session;
            const auto bytes = wireBytes(finish);
            m_deadlineUs = nowUs + m_config.finishTimeoutMs * US_PER_MS;
            enter(OtaPhase::VERIFYING);
            static_cast<void>(emit(bytes.data(), bytes.size()));
            return;
        }
        if (m_phase == OtaPhase::VERIFYING)
        {
            // A chunk acknowledgement in answer to OTA_FINISH means bytes
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
        if (!m_bundle.gitHash.empty() && !m_board.gitHash.empty())
        {
            return m_board.gitHash == m_bundle.gitHash;
        }
        // A hash-less image (never packaged, or a board that reports none)
        // leaves the version triplet as the only identity there is.
        return m_board.versionMajor == m_bundle.versionMajor &&
               m_board.versionMinor == m_bundle.versionMinor &&
               m_board.versionPatch == m_bundle.versionPatch;
    }

    bool OtaClient::boardRunsPrevious() const
    {
        if (!m_board.seen || !m_previousKnown)
        {
            return false;
        }
        if (!m_previousGitHash.empty() && !m_board.gitHash.empty())
        {
            return m_board.gitHash == m_previousGitHash;
        }
        return m_board.versionMajor == m_previousMajor && m_board.versionMinor == m_previousMinor &&
               m_board.versionPatch == m_previousPatch;
    }

    void OtaClient::fail(const std::string &reason)
    {
        m_lastError = reason;
        m_verdict = OtaVerdict::FAILED;
        m_confirmSent = false;
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
