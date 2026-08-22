#pragma once

/// @file
/// @brief THE alignment rule of the system: an estimated state (telemetry)
///        and an exact state (sim raw) describe the same instant when their
///        timestamps are near enough, and the estimator is scored on the
///        difference. Nearest-sample join, two-pointer, unmatched samples
///        dropped.
///
///        One implementation, two callers: the live path feeds it the last
///        few hundred milliseconds of both streams, the offline path feeds
///        it two whole recordings. Same samples in, same pairs out - a
///        number read on a live page and the same number read afterwards on
///        the recording have to agree, or neither is worth reading.
///
///        The arithmetic is double throughout, not because the wire is
///        (it is single-precision), but because this is a measurement of
///        that wire: the CSV cells are the widened values, and the reference
///        scoring reads them as such.

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace mark4
{
    /// Widest distance in time between two samples that still describe the
    /// same instant [us]. A telemetry sample with no exact state this close
    /// is dropped rather than compared against the wrong one.
    inline constexpr double MAX_ALIGN_GAP_US = 30000.0;

    /// Number of worst one-second windows reported per metric.
    inline constexpr std::size_t WORST_WINDOW_COUNT = 3U;

    /// One sample of either stream, reduced to what the comparison needs.
    struct AlignSample
    {
        double timestampUs = 0.0;             ///< instant the sample describes [us]
        std::array<double, 4> attitudeQuat{}; ///< attitude, w x y z
        double altitudeM = 0.0;               ///< altitude above the start [m]
        double verticalVelocityMps = 0.0;     ///< vertical velocity, up [m/s]
    };

    /// One telemetry sample and the exact sample nearest to it.
    struct AlignedPair
    {
        double timestampUs = 0.0;              ///< telemetry instant [us]
        double gapUs = 0.0;                    ///< exact instant minus this one [us]
        double attitudeErrorDeg = 0.0;         ///< rotation between the two attitudes [deg]
        double altitudeErrorM = 0.0;           ///< estimated minus exact altitude [m]
        double verticalVelocityErrorMps = 0.0; ///< estimated minus exact vertical speed [m/s]
    };

    /// One second of a run and how wrong one metric was over it.
    struct ScoreWindow
    {
        double startS = 0.0; ///< start of the window, on the run clock [s]
        double rms = 0.0;    ///< RMS of the metric over the window
    };

    /// How wrong one metric was over a whole run.
    struct MetricScore
    {
        std::string name;                      ///< metric name
        std::string unit;                      ///< unit the errors are in
        double rms = 0.0;                      ///< RMS over every aligned sample
        double max = 0.0;                      ///< largest absolute error
        std::vector<ScoreWindow> worstWindows; ///< worst seconds, worst first
    };

    /// What a whole comparison amounts to.
    struct CompareScore
    {
        std::size_t alignedSamples = 0U;      ///< telemetry samples that found a match
        std::size_t unmatched = 0U;           ///< telemetry samples that found none
        double durationS = 0.0;               ///< time the aligned samples span [s]
        std::array<MetricScore, 3> metrics{}; ///< attitude, altitude, vertical velocity
    };

    /// Which columns of a stream CSV carry the values the comparison needs.
    struct StreamColumns
    {
        std::size_t attitudeQuat = 0U;     ///< index of the quaternion w column
        std::size_t altitude = 0U;         ///< index of the altitude column
        std::size_t verticalVelocity = 0U; ///< index of the vertical velocity column
    };

    /// Columns of the telemetry CSV the recorder writes.
    inline constexpr StreamColumns TELEMETRY_COLUMNS{4U, 15U, 16U};

    /// Columns of the sim raw CSV the recorder writes: the exact altitude is
    /// the world z of the position, the exact vertical speed the world z of
    /// the velocity.
    inline constexpr StreamColumns SIM_RAW_COLUMNS{1U, 7U, 10U};

    /// @brief Rotation angle between two unit quaternions.
    /// @param first first attitude, w x y z
    /// @param second second attitude, w x y z
    /// @return the angle [deg]
    double errorAngleDeg(const std::array<double, 4> &first, const std::array<double, 4> &second);

    /// @brief Joins the two streams on their timestamps: every telemetry
    ///        sample takes the exact sample nearest to it, and is dropped
    ///        when the nearest one is further than MAX_ALIGN_GAP_US away.
    ///        Both streams must be in timestamp order; the walk is a single
    ///        forward pass over each.
    /// @param telemetry estimated states, in timestamp order
    /// @param simRaw exact states, in timestamp order
    /// @return one pair per matched telemetry sample, in the same order
    std::vector<AlignedPair> alignStreams(const std::vector<AlignSample> &telemetry,
                                          const std::vector<AlignSample> &simRaw);

    /// @brief Scores a whole set of pairs: RMS and worst error per metric,
    ///        plus the worst one-second windows, so a bad segment of a run
    ///        can be found instead of only measured.
    /// @param pairs aligned pairs, in timestamp order
    /// @param unmatched telemetry samples that found no match
    /// @return the score
    CompareScore scorePairs(const std::vector<AlignedPair> &pairs, std::size_t unmatched);

    /// The live side of the same join: both streams arrive packet by packet,
    /// and a telemetry sample is only compared once no exact sample still to
    /// come could be nearer to it than the one in hand. What comes out is
    /// therefore what alignStreams() would produce over the whole recording,
    /// pair for pair.
    class LiveAligner
    {
      public:
        /// How much of each stream is kept around [us]. Wide enough that the
        /// exact sample nearest to a telemetry sample is still in hand when
        /// that sample comes due.
        static constexpr double WINDOW_US = 200000.0;

        /// How far behind the newest exact sample a telemetry sample waits
        /// before being compared [us]. It is the alignment gap itself: past
        /// that distance, no exact sample yet to arrive can be nearer than
        /// the one already found. The resulting delay is what buys live and
        /// offline the same answer.
        static constexpr double DELAY_US = MAX_ALIGN_GAP_US;

        /// @brief Takes one estimated state.
        /// @param sample sample to buffer
        void onTelemetry(const AlignSample &sample);

        /// @brief Takes one exact state.
        /// @param sample sample to buffer
        void onSimRaw(const AlignSample &sample);

        /// @brief Compares every telemetry sample that has waited long
        ///        enough and forgets it.
        /// @return the pairs, oldest first
        std::vector<AlignedPair> takeDue();

      private:
        std::vector<AlignSample> m_telemetry; ///< estimated states not compared yet
        std::vector<AlignSample> m_simRaw;    ///< exact states still worth comparing against
    };

    /// @brief Reads one stream CSV into samples.
    /// @param path file to read
    /// @param columns where the values sit in a row
    /// @param samplesOut receives the samples, appended in file order
    /// @return true when the file could be read
    bool loadStreamSamples(const std::string &path,
                           const StreamColumns &columns,
                           std::vector<AlignSample> &samplesOut);
} // namespace mark4
