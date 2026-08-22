#pragma once

/// @file
/// @brief The ground side of the firmware update: one session state machine
///        driving one board through the transfer, the trial boot and the
///        confirmation of docs/ota-design.md.
///
///        The class owns no socket, no thread and no clock. It is fed time
///        by tick() and packets by onPacket(), and it emits packets through
///        a sink the composition root binds to whatever route reaches the
///        board (the framed serial link, or a command port for a desktop
///        flight process). That is what makes the whole flow - including
///        the timeouts, the go-back-N resends and the rollback verdict -
///        exercisable against a scripted fake board in a unit test.
///
///        The link stays shared throughout: OTA packets are one more packet
///        type on the same wire as telemetry and commands, and telemetry
///        keeps flowing between them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "hub/ota_bundle.hpp"
#include "protocol/ota.hpp"

namespace mark4
{
    /// Where a session stands. The four terminal phases (CONFIRMED,
    /// ROLLED_BACK, FAILED, and IDLE after a revert) hold until the next
    /// start(), so the operator reads the outcome instead of catching it.
    enum class OtaPhase : std::uint8_t
    {
        IDLE,          ///< nothing running
        QUERY,         ///< asking the board what it runs and from where
        ERASING,       ///< OTA_BEGIN sent, the board is erasing the target slot
        TRANSFER,      ///< streaming chunks, acknowledged by window
        VERIFYING,     ///< OTA_FINISH sent, the board is CRC-checking the slot
        REBOOTING,     ///< reboot command sent, waiting out the reset
        WAITING_BOARD, ///< polling status until the board talks again
        TESTING,       ///< the new image runs on trial, proving its link
        CONFIRMED,     ///< the trial image was confirmed, the update stands
        ROLLED_BACK,   ///< the bootloader brought the previous image back
        REVERTING,     ///< OTA_REVERT sent, going back to the other slot
        FAILED,        ///< the session ended on the reason in lastError()
    };

    /// The outcome an operator reads once a session is over.
    enum class OtaVerdict : std::uint8_t
    {
        NONE,        ///< nothing concluded yet
        CONFIRMED,   ///< the new firmware runs and was confirmed
        ROLLED_BACK, ///< the trial boot failed, the old firmware runs
        REVERTED,    ///< the operator asked for the other slot and got it
        FAILED,      ///< the update did not happen, see lastError()
    };

    /// Everything the last OtaStatusPacket said about the board.
    struct OtaBoardStatus
    {
        bool seen = false;                     ///< a status packet has arrived at least once
        std::uint64_t seenAtUs = 0U;           ///< when the last one arrived [us]
        std::uint8_t mcuId = 0U;               ///< OTA_MCU_* of the board
        std::uint8_t runningSlot = OTA_SLOT_A; ///< slot the running firmware executes from
        std::array<std::uint8_t, OTA_SLOT_COUNT> slotState = {OTA_SLOT_EMPTY,
                                                              OTA_SLOT_EMPTY}; ///< OTA_SLOT_*
        bool updaterBusy = false;        ///< a transfer session is open on the board
        std::uint8_t versionMajor = 0U;  ///< running firmware version
        std::uint8_t versionMinor = 0U;  ///< running firmware version
        std::uint8_t versionPatch = 0U;  ///< running firmware version
        std::string gitHash;             ///< running image git hash, empty when unstamped
        std::uint32_t slotSize = 0U;     ///< bytes available per slot
        std::uint16_t maxChunkData = 0U; ///< largest chunk data size the board accepts
    };

    /// How far the transfer has got. ackedBytes is what the board has
    /// written and is therefore what a progress bar must show; sentBytes
    /// runs ahead of it by up to one window.
    struct OtaProgress
    {
        std::uint32_t sentBytes = 0U;  ///< bytes handed to the link
        std::uint32_t ackedBytes = 0U; ///< bytes the board acknowledged writing
        std::uint32_t totalBytes = 0U; ///< image bytes to transfer, 0 outside a session
        std::uint32_t retries = 0U;    ///< go-back-N resends of the current session
    };

    /// @brief Names one phase, in the camelCase the websocket messages use.
    /// @param phase phase to name
    /// @return static name
    [[nodiscard]] const char *otaPhaseName(OtaPhase phase);

    /// @brief Names one verdict, in the camelCase the messages use.
    /// @param verdict verdict to name
    /// @return static name
    [[nodiscard]] const char *otaVerdictName(OtaVerdict verdict);

    /// @brief Names one slot state of protocol/ota.hpp.
    /// @param state one of the OTA_SLOT_* values
    /// @return static name, "unknown" outside the enumeration
    [[nodiscard]] const char *otaSlotStateName(std::uint8_t state);

    /// @brief Turns one OtaAckPacket result byte into the sentence the
    ///        operator reads. A refusal code is useless on a page; the
    ///        reason behind it is the whole point of having codes.
    /// @param result one of the OTA_RESULT_* values
    /// @return the reason, in plain words
    [[nodiscard]] std::string otaResultText(std::uint8_t result);

    /// One update session at a time, against one board.
    class OtaClient
    {
      public:
        /// How long the OtaAckPacket of an OTA_BEGIN may take [ms]. Erasing
        /// a 384 KB slot on an F405 is seconds of stalled flash.
        static constexpr std::uint64_t BEGIN_TIMEOUT_MS = 15000U;

        /// How long the OtaAckPacket of an OTA_FINISH may take [ms]: one
        /// CRC pass over the slot, milliseconds on the hardware unit.
        static constexpr std::uint64_t FINISH_TIMEOUT_MS = 5000U;

        /// Chunk-acknowledgement silence after which the sender goes back to
        /// the last acknowledged offset [ms].
        static constexpr std::uint64_t CHUNK_ACK_TIMEOUT_MS = 500U;

        /// How long a confirm, revert or abort acknowledgement may take [ms].
        static constexpr std::uint64_t ACK_TIMEOUT_MS = 2000U;

        /// Period of the status polling, on the initial query and while
        /// waiting for the board to come back [ms].
        static constexpr std::uint64_t STATUS_PERIOD_MS = 1000U;

        /// Status requests the initial query sends before giving up.
        static constexpr std::uint32_t STATUS_TRIES = 5U;

        /// Silence granted to the reset itself before a status answer is
        /// believed [ms]: right after the reboot command the old image can
        /// still answer one request, and that answer must not be read as a
        /// rollback.
        static constexpr std::uint64_t REBOOT_SETTLE_MS = 1500U;

        /// How long the board may take to come back after the reboot [ms].
        static constexpr std::uint64_t BOARD_RETURN_TIMEOUT_MS = 30000U;

        /// Healthy link the trial image must show before it is confirmed [ms].
        static constexpr std::uint64_t HEALTHY_LINK_MS = 3000U;

        /// Status answers the trial image must give over that window.
        static constexpr std::uint32_t HEALTHY_STATUSES = 3U;

        /// Go-back-N resends of one session before it is declared lost.
        static constexpr std::uint32_t MAX_RETRIES = 8U;

        /// Sends of one acknowledged request before it is declared lost.
        static constexpr std::uint32_t MAX_ACK_TRIES = 3U;

        /// Delay between two chunk sends [us]. The hub reaches the board
        /// over WiFi and the board over a 921600 baud UART: without this,
        /// one burst of a window outruns the bridge's transmit side.
        static constexpr std::uint64_t CHUNK_DELAY_US = 2000U;

        /// Everything a caller may want to bend, mostly for the tests.
        struct Config
        {
            std::uint64_t beginTimeoutMs = BEGIN_TIMEOUT_MS;        ///< erase acknowledgement
            std::uint64_t finishTimeoutMs = FINISH_TIMEOUT_MS;      ///< staging acknowledgement
            std::uint64_t chunkAckTimeoutMs = CHUNK_ACK_TIMEOUT_MS; ///< chunk-ack silence
            std::uint64_t ackTimeoutMs = ACK_TIMEOUT_MS;            ///< confirm, revert, abort
            std::uint64_t statusPeriodMs = STATUS_PERIOD_MS;        ///< status polling period
            std::uint64_t rebootSettleMs = REBOOT_SETTLE_MS;        ///< silence after the reset
            std::uint64_t boardReturnTimeoutMs = BOARD_RETURN_TIMEOUT_MS; ///< reappearance budget
            std::uint64_t healthyLinkMs = HEALTHY_LINK_MS;                ///< trial proof window
            std::uint64_t chunkDelayUs = CHUNK_DELAY_US;                  ///< inter-chunk pacing
            std::uint32_t statusTries = STATUS_TRIES;         ///< initial query attempts
            std::uint32_t healthyStatuses = HEALTHY_STATUSES; ///< trial proof answers
            std::uint32_t maxRetries = MAX_RETRIES;           ///< go-back-N budget
            std::uint32_t maxAckTries = MAX_ACK_TRIES;        ///< request resend budget
            bool autoConfirm = true;                          ///< confirm without an operator
        };

        /// Route one packet to the board. Returns false and fills the reason
        /// when the board is not reachable, which fails the session.
        using PacketSink = std::function<bool(const std::uint8_t *, std::size_t, std::string &)>;

        /// Called whenever what status() would report has changed, so the
        /// composition root can publish a progress event without polling.
        using ChangeHandler = std::function<void()>;

        OtaClient() = default;

        /// @param config settings of this client
        explicit OtaClient(Config config);

        /// @brief Sets the route packets go out by. Until then, any request
        ///        that would send something is refused.
        /// @param sink route to the board
        void setSink(PacketSink sink);

        /// @brief Sets the callback fired on every observable change.
        /// @param handler callback, or an empty function to remove it
        void setOnChange(ChangeHandler handler);

        /// @brief Sets the bundle a start() with no path of its own uses:
        ///        the standard build output, so the common case is one click.
        /// @param path bundle path
        void setDefaultBundlePath(std::string path);

        /// @brief Opens a session: loads and validates the bundle, then asks
        ///        the board what it runs. Everything that depends on the
        ///        board (which slot, which image, does the chip match) is
        ///        decided when the answer arrives.
        /// @param bundlePath bundle to send, empty for the default one
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the reason on refusal
        /// @return true when the session opened
        [[nodiscard]] bool start(const std::string &bundlePath,
                                 std::uint64_t nowUs,
                                 std::string &errorOut);

        /// @brief Drops the session. Tells the board when one was open there,
        ///        so its half-written slot is released instead of timing out.
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the reason on refusal
        /// @return true when the session was dropped
        [[nodiscard]] bool abortSession(std::uint64_t nowUs, std::string &errorOut);

        /// @brief Confirms the running trial image by hand, which is what
        ///        the manual-confirm mode replaces the automatic send with.
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the reason on refusal
        /// @return true when the confirmation went out
        [[nodiscard]] bool confirm(std::uint64_t nowUs, std::string &errorOut);

        /// @brief Asks the board to activate its other slot, then reboots it.
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the reason on refusal
        /// @return true when the request went out
        [[nodiscard]] bool revert(std::uint64_t nowUs, std::string &errorOut);

        /// @brief Asks the board for one status packet, outside any session:
        ///        this is how a page shows what the board runs before an
        ///        update is even considered.
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the reason on refusal
        /// @return true when the request went out
        [[nodiscard]] bool requestBoardStatus(std::uint64_t nowUs, std::string &errorOut);

        /// @brief Switches between confirming on the hub's own judgment and
        ///        waiting for an operator.
        /// @param on true to confirm automatically
        void setAutoConfirm(bool on);

        /// @brief Consumes one packet read from the link.
        /// @param data packet bytes
        /// @param size packet size in bytes
        /// @param nowUs current time [us]
        /// @return true when the packet was one of the updater's, whether or
        ///         not it changed anything: the caller must not then count it
        ///         as an undecodable frame
        [[nodiscard]] bool onPacket(const std::uint8_t *data,
                                    std::size_t size,
                                    std::uint64_t nowUs);

        /// @brief Runs the timeouts and the chunk pacing. Cheap when idle.
        /// @param nowUs current time [us]
        void tick(std::uint64_t nowUs);

        /// @return true while a session occupies the link, so the poll loop
        ///         can tighten its cadence for the duration
        [[nodiscard]] bool busy() const;

        /// @return where the session stands
        [[nodiscard]] OtaPhase phase() const
        {
            return m_phase;
        }

        /// @return what the last finished session concluded
        [[nodiscard]] OtaVerdict verdict() const
        {
            return m_verdict;
        }

        /// @return the reason of the last failure or refusal, empty when none
        [[nodiscard]] const std::string &lastError() const
        {
            return m_lastError;
        }

        /// @return the verdict as one sentence for the operator, empty while
        ///         nothing is concluded
        [[nodiscard]] std::string verdictText() const;

        /// @return true when the hub confirms on its own judgment
        [[nodiscard]] bool autoConfirm() const
        {
            return m_config.autoConfirm;
        }

        /// @return true when the trial image has proven its link and only an
        ///         operator gesture is missing
        [[nodiscard]] bool confirmReady() const;

        /// @return the bundle currently loaded, empty when none is
        [[nodiscard]] const OtaBundle &bundle() const
        {
            return m_bundle;
        }

        /// @return the bundle path a start() would use next
        [[nodiscard]] const std::string &bundlePath() const
        {
            return m_bundlePath;
        }

        /// @return what the board last said about itself
        [[nodiscard]] const OtaBoardStatus &board() const
        {
            return m_board;
        }

        /// @return how far the transfer has got
        [[nodiscard]] const OtaProgress &progress() const
        {
            return m_progress;
        }

        /// @return the slot the session is filling, OTA_SLOT_COUNT when none
        [[nodiscard]] std::uint8_t targetSlot() const
        {
            return m_targetSlot;
        }

      private:
        /// @brief Sends one wire packet through the sink, failing the session
        ///        when the route is gone.
        /// @param data packet bytes
        /// @param size packet size in bytes
        /// @return true when the bytes went out
        bool emit(const std::uint8_t *data, std::size_t size);

        /// @brief Sends one OTA_STATUS_REQUEST.
        /// @return true when it went out
        bool sendStatusRequest();

        /// @brief Sends the reboot command the trial boot needs. It is the
        ///        existing command, not an updater packet: nothing in the
        ///        protocol had to move for the update path.
        /// @return true when it went out
        bool sendReboot();

        /// @brief Sends one chunk at the current send offset.
        /// @return true when it went out
        bool sendChunk();

        /// @brief Hands the link as many chunks as the window and the pacing
        ///        allow, then arms the chunk-ack timeout.
        /// @param nowUs current time [us]
        void pumpChunks(std::uint64_t nowUs);

        /// @brief Decides what to send once the board has described itself,
        ///        and opens the transfer.
        /// @param nowUs current time [us]
        void openTransfer(std::uint64_t nowUs);

        /// @brief Reads one OtaStatusPacket into the board snapshot and
        ///        advances whatever phase was waiting for it.
        /// @param data packet bytes
        /// @param nowUs current time [us]
        void onStatus(const std::uint8_t *data, std::uint64_t nowUs);

        /// @brief Applies one OtaAckPacket.
        /// @param data packet bytes
        /// @param nowUs current time [us]
        void onAck(const std::uint8_t *data, std::uint64_t nowUs);

        /// @brief Applies one OtaChunkAckPacket.
        /// @param data packet bytes
        /// @param nowUs current time [us]
        void onChunkAck(const std::uint8_t *data, std::uint64_t nowUs);

        /// @brief Judges the board that just came back after a trial boot.
        /// @param nowUs current time [us]
        void judgeReturn(std::uint64_t nowUs);

        /// @brief Sends the confirmation and arms its acknowledgement timeout.
        /// @param nowUs current time [us]
        void sendConfirm(std::uint64_t nowUs);

        /// @brief Ends the session on a reason.
        /// @param reason what went wrong, in plain words
        void fail(const std::string &reason);

        /// @brief Moves to a phase and reports the change.
        /// @param phase phase to move to
        void enter(OtaPhase phase);

        /// @brief Reports that what status() would say has changed.
        void notifyChange();

        /// @return true when the board's identity is the one the bundle holds
        [[nodiscard]] bool boardRunsBundle() const;

        /// @return true when the board's identity is the one it had before
        ///         the update started
        [[nodiscard]] bool boardRunsPrevious() const;

        Config m_config;                            ///< settings of this client
        PacketSink m_sink;                          ///< route to the board
        ChangeHandler m_onChange;                   ///< observable-change callback
        std::string m_defaultBundlePath;            ///< bundle a pathless start uses
        std::string m_bundlePath;                   ///< bundle the session is sending
        OtaBundle m_bundle;                         ///< loaded bundle
        OtaBoardStatus m_board;                     ///< last status the board sent
        OtaProgress m_progress;                     ///< transfer progress
        OtaPhase m_phase = OtaPhase::IDLE;          ///< where the session stands
        OtaVerdict m_verdict = OtaVerdict::NONE;    ///< outcome of the last session
        std::string m_lastError;                    ///< reason of the last failure
        std::uint32_t m_session = 0U;               ///< session nonce of the wire
        std::uint8_t m_targetSlot = OTA_SLOT_COUNT; ///< slot being filled
        std::uint32_t m_chunkData = 0U;             ///< data bytes per chunk this session
        std::uint64_t m_deadlineUs = 0U;            ///< phase timeout instant [us]
        std::uint64_t m_nextStatusUs = 0U;          ///< next status request instant [us]
        std::uint64_t m_nextChunkUs = 0U;           ///< earliest next chunk send [us]
        std::uint64_t m_chunkDeadlineUs = 0U;       ///< chunk-ack silence deadline [us]
        std::uint64_t m_settleUntilUs = 0U;         ///< status answers ignored until [us]
        std::uint64_t m_healthySinceUs = 0U;        ///< first trial status instant [us]
        std::uint32_t m_healthyCount = 0U;          ///< trial status answers seen
        std::uint32_t m_statusTries = 0U;           ///< status requests of this phase
        std::uint32_t m_ackTries = 0U;              ///< sends of the current request
        bool m_confirmSent = false;                 ///< a confirmation is outstanding
        bool m_previousKnown = false;               ///< the pre-update identity was captured
        std::uint8_t m_previousMajor = 0U;          ///< version the board ran before
        std::uint8_t m_previousMinor = 0U;          ///< version the board ran before
        std::uint8_t m_previousPatch = 0U;          ///< version the board ran before
        std::string m_previousGitHash;              ///< hash the board ran before
    };
} // namespace mark4
