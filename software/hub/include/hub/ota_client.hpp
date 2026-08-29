#pragma once

/// @file
/// @brief The ground side of the firmware update: one session state machine
///        driving one board through the transfer, the trial boot and the
///        confirmation of docs/ota-design.md.
///
///        The class owns no socket, no thread and no clock. It is fed time
///        by tick() and messages by onEnvelope(), and it emits messages
///        through a sink the composition root binds to whatever route reaches
///        the board (the framed serial link, or the transport node of a
///        desktop flight process). That is what makes the whole flow -
///        including the timeouts, the go-back-N resends and the rollback
///        verdict - exercisable against a scripted fake board in a unit test.
///
///        The link stays shared throughout: the updater messages are one more
///        body of the same Envelope as telemetry and commands, and telemetry
///        keeps flowing between them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "hub/ota_bundle.hpp"
#include "protocol/envelope.hpp"
#include "protocol/ota_image.hpp"

namespace mark4
{
    /// Where a session stands. The four terminal phases (CONFIRMED,
    /// ROLLED_BACK, FAILED, and IDLE after a revert) hold until the next
    /// start(), so the operator reads the outcome instead of catching it.
    enum class OtaPhase : std::uint8_t
    {
        IDLE,          ///< nothing running
        QUERY,         ///< asking the board what it runs and from where
        ERASING,       ///< OtaBegin sent, the board is erasing the target slot
        TRANSFER,      ///< streaming chunks, acknowledged by window
        VERIFYING,     ///< OtaFinish sent, the board is CRC-checking the slot
        REBOOTING,     ///< reboot command sent, waiting out the reset
        WAITING_BOARD, ///< polling status until the board talks again
        TESTING,       ///< the new image runs on trial, proving its link
        CONFIRMED,     ///< the trial image was confirmed, the update stands
        ROLLED_BACK,   ///< the bootloader brought the previous image back
        REVERTING,     ///< OtaRevert sent, going back to the other slot
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

    /// One firmware slot as the board reports it: state plus the identity
    /// of whatever image sits in it.
    struct OtaSlotInfo
    {
        mark4_OtaSlotState state = mark4_OtaSlotState_EMPTY; ///< lifecycle state
        std::uint32_t buildEpoch = 0U; ///< image build epoch; 0 with no header,
                                       ///< OTA_IMAGE_UNSTAMPED unpackaged
        std::string gitHash;           ///< image git hash, empty when unstamped
    };

    /// Everything the last OtaStatus said about the board.
    struct OtaBoardStatus
    {
        bool seen = false;                             ///< a status has arrived at least once
        std::uint64_t seenAtUs = 0U;                   ///< when the last one arrived [us]
        mark4_Mcu mcu = mark4_Mcu_MCU_UNSPECIFIED;     ///< chip of the board
        std::uint8_t runningSlot = OTA_SLOT_A;         ///< slot the running firmware executes from
        std::uint8_t activeSlot = OTA_SLOT_A;          ///< slot the boot metadata prefers
        std::array<OtaSlotInfo, OTA_SLOT_COUNT> slots; ///< state and identity per slot
        bool updaterBusy = false;                      ///< a transfer session is open on the board
        std::uint32_t buildEpoch = 0U;   ///< running image build epoch, the build's identity;
                                         ///< 0 with no header, OTA_IMAGE_UNSTAMPED unpackaged
        std::string gitHash;             ///< running image git hash, empty when unstamped
        std::uint32_t slotSize = 0U;     ///< bytes available per slot
        std::uint32_t maxChunkData = 0U; ///< largest chunk data size the board accepts
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

    /// @brief Names one slot state.
    /// @param state wire value
    /// @return static name, "unknown" outside the enumeration
    [[nodiscard]] const char *otaSlotStateName(mark4_OtaSlotState state);

    /// @brief Turns one OtaAck result into the sentence the operator reads. A
    ///        refusal code is useless on a page; the reason behind it is the
    ///        whole point of having codes.
    /// @param result wire value
    /// @return the reason, in plain words
    [[nodiscard]] std::string otaResultText(mark4_OtaResult result);

    /// One update session at a time, against one board.
    class OtaClient
    {
      public:
        /// How long the OtaAck of an OtaBegin may take [ms]. Erasing a 384 KB
        /// slot on an F405 is seconds of stalled flash.
        static constexpr std::uint64_t BEGIN_TIMEOUT_MS = 15000U;

        /// How long the OtaAck of an OtaFinish may take [ms]: one CRC pass
        /// over the slot, milliseconds on the hardware unit.
        static constexpr std::uint64_t FINISH_TIMEOUT_MS = 5000U;

        /// Chunk-acknowledgement silence after which the sender goes back to
        /// the last acknowledged offset [ms].
        static constexpr std::uint64_t CHUNK_ACK_TIMEOUT_MS = 500U;

        /// How long a revert or abort acknowledgement may take [ms].
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

        /// Period of the bundle freshness check while no session runs [ms]:
        /// a rebuild writes a new file, and the page must show the new
        /// build without anyone telling the hub anything.
        static constexpr std::uint64_t BUNDLE_CHECK_MS = 1000U;

        /// Go-back-N resends of one session before it is declared lost.
        static constexpr std::uint32_t MAX_RETRIES = 8U;

        /// Sends of one acknowledged request before it is declared lost.
        static constexpr std::uint32_t MAX_ACK_TRIES = 3U;

        /// Delay between two chunk sends [us]. The hub reaches the board
        /// over WiFi and the board over a 921600 baud UART: the pacing
        /// must stay under that wire's ~92 KB/s or the bridge's transmit
        /// side overflows. A 256-byte chunk frame takes ~2.8 ms on the
        /// UART; at 2 ms the bench lost about one chunk per window to the
        /// overflow (go-back-N absorbed it, at ~14 retries per image).
        static constexpr std::uint64_t CHUNK_DELAY_US = 3000U;

        /// Everything a caller may want to bend, mostly for the tests.
        struct Config
        {
            std::uint64_t beginTimeoutMs = BEGIN_TIMEOUT_MS;        ///< erase acknowledgement
            std::uint64_t finishTimeoutMs = FINISH_TIMEOUT_MS;      ///< staging acknowledgement
            std::uint64_t chunkAckTimeoutMs = CHUNK_ACK_TIMEOUT_MS; ///< chunk-ack silence
            std::uint64_t ackTimeoutMs = ACK_TIMEOUT_MS;            ///< revert, abort
            std::uint64_t statusPeriodMs = STATUS_PERIOD_MS;        ///< status polling period
            std::uint64_t rebootSettleMs = REBOOT_SETTLE_MS;        ///< silence after the reset
            std::uint64_t boardReturnTimeoutMs = BOARD_RETURN_TIMEOUT_MS; ///< reappearance budget
            std::uint64_t chunkDelayUs = CHUNK_DELAY_US;                  ///< inter-chunk pacing
            std::uint32_t statusTries = STATUS_TRIES;  ///< initial query attempts
            std::uint32_t maxRetries = MAX_RETRIES;    ///< go-back-N budget
            std::uint32_t maxAckTries = MAX_ACK_TRIES; ///< request resend budget
        };

        /// Route one message to the board. Returns false and fills the
        /// reason when the board is not reachable, which fails the session.
        using MessageSink = std::function<bool(const mark4_Envelope &, std::string &)>;

        /// Called whenever what status() would report has changed, so the
        /// composition root can publish a progress event without polling.
        using ChangeHandler = std::function<void()>;

        OtaClient() = default;

        /// @param config settings of this client
        explicit OtaClient(Config config);

        /// @brief Sets the route messages go out by. Until then, any request
        ///        that would send something is refused.
        /// @param sink route to the board
        void setSink(MessageSink sink);

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

        /// @brief Asks the board to activate its other slot, then reboots it.
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the reason on refusal
        /// @return true when the request went out
        [[nodiscard]] bool revert(std::uint64_t nowUs, std::string &errorOut);

        /// @brief Asks the board for one status message, outside any session:
        ///        this is how a page shows what the board runs before an
        ///        update is even considered.
        /// @param nowUs current time [us]
        /// @param[out] errorOut receives the reason on refusal
        /// @return true when the request went out
        [[nodiscard]] bool requestBoardStatus(std::uint64_t nowUs, std::string &errorOut);

        /// @brief Consumes one message read from the link.
        /// @param envelope decoded message
        /// @param nowUs current time [us]
        /// @return true when the message was one of the updater's, whether or
        ///         not it changed anything
        [[nodiscard]] bool onEnvelope(const mark4_Envelope &envelope, std::uint64_t nowUs);

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
        /// @brief Reloads the bundle when its file changed on disk (a
        ///        rebuild), loads it the first time it appears, and forgets
        ///        it when the file goes away. Never touches a running
        ///        session: the transfer reads the loaded images.
        /// @param nowUs monotonic time [us]
        void refreshBundle(std::uint64_t nowUs);

        /// @brief Sends one message through the sink, failing the session
        ///        when the route is gone.
        /// @param envelope message to send
        /// @return true when it went out
        bool emit(const mark4_Envelope &envelope);

        /// @brief Sends one OtaStatusRequest.
        /// @return true when it went out
        bool sendStatusRequest();

        /// @brief Sends the reboot command the trial boot needs. It is the
        ///        existing command, not an updater message: nothing in the
        ///        wire had to move for the update path.
        /// @return true when it went out
        bool sendReboot();

        /// @brief Sends the sessionless revert request.
        /// @return true when it went out
        bool sendRevert();

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

        /// @brief Reads one OtaStatus into the board snapshot and advances
        ///        whatever phase was waiting for it.
        /// @param status decoded message
        /// @param nowUs current time [us]
        void onStatus(const mark4_OtaStatus &status, std::uint64_t nowUs);

        /// @brief Applies one OtaAck.
        /// @param ack decoded message
        /// @param nowUs current time [us]
        void onAck(const mark4_OtaAck &ack, std::uint64_t nowUs);

        /// @brief Applies one OtaChunkAck.
        /// @param ack decoded message
        /// @param nowUs current time [us]
        void onChunkAck(const mark4_OtaChunkAck &ack, std::uint64_t nowUs);

        /// @brief Judges the board that just came back after a trial boot.
        /// @param nowUs current time [us]
        void judgeReturn(std::uint64_t nowUs);

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

        Config m_config;                                    ///< settings of this client
        MessageSink m_sink;                                 ///< route to the board
        ChangeHandler m_onChange;                           ///< observable-change callback
        std::string m_defaultBundlePath;                    ///< bundle a pathless start uses
        std::string m_bundlePath;                           ///< bundle the session is sending
        OtaBundle m_bundle;                                 ///< loaded bundle
        std::filesystem::file_time_type m_bundleFileTime{}; ///< mtime of the loaded file
        std::uintmax_t m_bundleFileSize = 0U;               ///< size of the loaded file
        std::uint64_t m_nextBundleCheckUs = 0U;             ///< next freshness check [us]
        OtaBoardStatus m_board;                             ///< last status the board sent
        OtaProgress m_progress;                             ///< transfer progress
        OtaPhase m_phase = OtaPhase::IDLE;                  ///< where the session stands
        OtaVerdict m_verdict = OtaVerdict::NONE;            ///< outcome of the last session
        std::string m_lastError;                            ///< reason of the last failure
        std::uint32_t m_session = 0U;                       ///< session nonce of the wire
        std::uint8_t m_targetSlot = OTA_SLOT_COUNT;         ///< slot being filled
        std::uint32_t m_chunkData = 0U;                     ///< data bytes per chunk this session
        std::uint64_t m_deadlineUs = 0U;                    ///< phase timeout instant [us]
        std::uint64_t m_nextStatusUs = 0U;                  ///< next status request instant [us]
        std::uint64_t m_nextChunkUs = 0U;                   ///< earliest next chunk send [us]
        std::uint64_t m_chunkDeadlineUs = 0U;               ///< chunk-ack silence deadline [us]
        std::uint32_t m_stalledRounds = 0U;                 ///< resend rounds with no progress
        std::uint32_t m_resendOffset = 0U;                  ///< offset the last resend answered
        std::uint64_t m_resendGuardUs = 0U;      ///< ignore repeats of it until then [us]
        std::uint64_t m_settleUntilUs = 0U;      ///< status answers ignored until [us]
        std::uint32_t m_statusTries = 0U;        ///< status requests of this phase
        std::uint32_t m_ackTries = 0U;           ///< sends of the current request
        bool m_previousKnown = false;            ///< the pre-update identity was captured
        std::uint32_t m_previousBuildEpoch = 0U; ///< build epoch the board ran before
    };
} // namespace mark4
