/// @file
/// @brief Stream alignment and scoring implementation.

#include "hub/stream_align.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numbers>

namespace mark4
{
    namespace
    {
        /// Microseconds to seconds. A multiplication, not a division by
        /// 1e6: the two do not always land on the same last bit, and the
        /// reference scoring this reproduces multiplies.
        constexpr double US_TO_S = 1e-6;

        /// Radians to degrees, spelled as the reference scoring spells it.
        constexpr double RAD_TO_DEG = 180.0 / std::numbers::pi;

        /// A quaternion turns by twice the angle it carries.
        constexpr double HALF_ANGLE = 2.0;

        /// Compensated running sum. Adding thousands of squares naively
        /// drops the small ones under the running total, and the loss lands
        /// where it hurts most: near a perfect attitude match the arc cosine
        /// magnifies the last bits of its argument several hundredfold, so a
        /// dot product summed carelessly moves the reported angle in its
        /// eleventh digit. Carrying the rounding error along costs three
        /// operations and removes that.
        class CompensatedSum
        {
          public:
            /// @brief Adds one term.
            /// @param value term to add
            void add(double value)
            {
                const double moved = m_total + value;
                if (std::fabs(m_total) >= std::fabs(value))
                {
                    m_lost += (m_total - moved) + value;
                }
                else
                {
                    m_lost += (value - moved) + m_total;
                }
                m_total = moved;
            }

            /// @return the sum, with the rounding error it dropped folded in
            [[nodiscard]] double total() const
            {
                return m_total + m_lost;
            }

          private:
            double m_total = 0.0; ///< running sum
            double m_lost = 0.0;  ///< rounding error the running sum dropped
        };

        /// One second of run, holding what the windows need.
        struct Bucket
        {
            long second = 0;                            ///< whole seconds since the first pair
            std::size_t count = 0U;                     ///< pairs that fell in it
            std::array<CompensatedSum, 3> sumSquares{}; ///< squared errors summed, per metric
        };

        /// @brief Reads one metric out of a pair, by its index in the score.
        /// @param pair pair to read
        /// @param metric metric index: 0 attitude, 1 altitude, 2 vertical velocity
        /// @return the error
        double metricOf(const AlignedPair &pair, std::size_t metric)
        {
            if (metric == 0U)
            {
                return pair.attitudeErrorDeg;
            }
            if (metric == 1U)
            {
                return pair.altitudeErrorM;
            }
            return pair.verticalVelocityErrorMps;
        }

        /// @brief Splits one CSV line on its commas.
        /// @param line line to split, terminator already removed
        /// @return the cells, as they appear
        std::vector<std::string> csvCells(const std::string &line)
        {
            std::vector<std::string> cells;
            std::size_t start = 0U;
            while (start <= line.size())
            {
                const std::size_t comma = line.find(',', start);
                if (comma == std::string::npos)
                {
                    cells.push_back(line.substr(start));
                    break;
                }
                cells.push_back(line.substr(start, comma - start));
                start = comma + 1U;
            }
            return cells;
        }

        /// @brief Reads one cell as a number.
        /// @param cell text to read
        /// @return the value, 0 when the cell is not a number
        double cellValue(const std::string &cell)
        {
            return std::strtod(cell.c_str(), nullptr);
        }

        /// @brief Drops the samples that have fallen out of the live window.
        /// @param samples buffer to trim
        void trimWindow(std::vector<AlignSample> &samples)
        {
            if (samples.empty())
            {
                return;
            }
            const double oldest = samples.back().timestampUs - LiveAligner::WINDOW_US;
            const auto first =
                std::find_if(samples.begin(), samples.end(), [oldest](const AlignSample &sample) {
                    return sample.timestampUs >= oldest;
                });
            static_cast<void>(samples.erase(samples.begin(), first));
        }
    } // namespace

    double errorAngleDeg(const std::array<double, 4> &first, const std::array<double, 4> &second)
    {
        CompensatedSum dot;
        for (std::size_t index = 0U; index < first.size(); ++index)
        {
            dot.add(first[index] * second[index]);
        }
        // A quaternion and its negation are the same rotation, hence the
        // absolute value; the clamp keeps a unit-norm rounding error out of
        // the domain of acos.
        return RAD_TO_DEG * HALF_ANGLE * std::acos(std::min(1.0, std::fabs(dot.total())));
    }

    std::vector<AlignedPair> alignStreams(const std::vector<AlignSample> &telemetry,
                                          const std::vector<AlignSample> &simRaw)
    {
        std::vector<AlignedPair> pairs;
        if (simRaw.empty())
        {
            return pairs;
        }
        pairs.reserve(telemetry.size());

        std::size_t rawIndex = 0U;
        for (const AlignSample &sample : telemetry)
        {
            // The pointer only ever moves forward: both streams are in
            // timestamp order, so the nearest exact sample of the next
            // telemetry sample is never behind the current one.
            while (rawIndex + 1U < simRaw.size() &&
                   std::fabs(simRaw[rawIndex + 1U].timestampUs - sample.timestampUs) <=
                       std::fabs(simRaw[rawIndex].timestampUs - sample.timestampUs))
            {
                ++rawIndex;
            }
            const AlignSample &nearest = simRaw[rawIndex];
            const double gap = nearest.timestampUs - sample.timestampUs;
            if (std::fabs(gap) > MAX_ALIGN_GAP_US)
            {
                continue;
            }
            AlignedPair pair;
            pair.timestampUs = sample.timestampUs;
            pair.gapUs = gap;
            pair.attitudeErrorDeg = errorAngleDeg(sample.attitudeQuat, nearest.attitudeQuat);
            pair.altitudeErrorM = sample.altitudeM - nearest.altitudeM;
            pair.verticalVelocityErrorMps =
                sample.verticalVelocityMps - nearest.verticalVelocityMps;
            pairs.push_back(pair);
        }
        return pairs;
    }

    CompareScore scorePairs(const std::vector<AlignedPair> &pairs, std::size_t unmatched)
    {
        CompareScore score;
        score.alignedSamples = pairs.size();
        score.unmatched = unmatched;
        score.metrics[0].name = "attitude";
        score.metrics[0].unit = "deg";
        score.metrics[1].name = "altitude";
        score.metrics[1].unit = "m";
        score.metrics[2].name = "verticalVelocity";
        score.metrics[2].unit = "m/s";
        if (pairs.empty())
        {
            return score;
        }

        const double startS = pairs.front().timestampUs * US_TO_S;
        score.durationS = pairs.back().timestampUs * US_TO_S - startS;

        std::vector<Bucket> buckets;
        std::array<CompensatedSum, 3> sumSquares{};
        std::array<double, 3> worst{};
        for (const AlignedPair &pair : pairs)
        {
            const auto second = static_cast<long>(pair.timestampUs * US_TO_S - startS);
            if (buckets.empty() || buckets.back().second != second)
            {
                Bucket fresh;
                fresh.second = second;
                buckets.push_back(fresh);
            }
            Bucket &bucket = buckets.back();
            ++bucket.count;
            for (std::size_t metric = 0U; metric < sumSquares.size(); ++metric)
            {
                const double value = metricOf(pair, metric);
                sumSquares[metric].add(value * value);
                bucket.sumSquares[metric].add(value * value);
                worst[metric] = std::max(worst[metric], std::fabs(value));
            }
        }

        for (std::size_t metric = 0U; metric < sumSquares.size(); ++metric)
        {
            MetricScore &result = score.metrics[metric];
            result.rms = std::sqrt(sumSquares[metric].total() / static_cast<double>(pairs.size()));
            result.max = worst[metric];

            std::vector<ScoreWindow> windows;
            windows.reserve(buckets.size());
            for (const Bucket &bucket : buckets)
            {
                ScoreWindow window;
                window.startS = startS + static_cast<double>(bucket.second);
                window.rms = std::sqrt(bucket.sumSquares[metric].total() /
                                       static_cast<double>(bucket.count));
                windows.push_back(window);
            }
            // Worst first, and the later second first among equals: two
            // windows scoring the same is a tie the run's own order breaks.
            std::sort(windows.begin(),
                      windows.end(),
                      [](const ScoreWindow &left, const ScoreWindow &right) {
                          if (left.rms != right.rms)
                          {
                              return left.rms > right.rms;
                          }
                          return left.startS > right.startS;
                      });
            if (windows.size() > WORST_WINDOW_COUNT)
            {
                windows.resize(WORST_WINDOW_COUNT);
            }
            result.worstWindows = windows;
        }
        return score;
    }

    void LiveAligner::onTelemetry(const AlignSample &sample)
    {
        m_telemetry.push_back(sample);
        // Without an exact stream the pending side would otherwise grow with
        // the run: nothing will ever come to match it.
        trimWindow(m_telemetry);
    }

    void LiveAligner::onSimRaw(const AlignSample &sample)
    {
        m_simRaw.push_back(sample);
        trimWindow(m_simRaw);
    }

    std::vector<AlignedPair> LiveAligner::takeDue()
    {
        if (m_simRaw.empty() || m_telemetry.empty())
        {
            return {};
        }
        const double cutoff = m_simRaw.back().timestampUs - DELAY_US;
        const auto end =
            std::find_if(m_telemetry.begin(), m_telemetry.end(), [cutoff](const AlignSample &s) {
                return s.timestampUs > cutoff;
            });
        if (end == m_telemetry.begin())
        {
            return {};
        }
        const std::vector<AlignSample> due(m_telemetry.begin(), end);
        static_cast<void>(m_telemetry.erase(m_telemetry.begin(), end));
        return alignStreams(due, m_simRaw);
    }

    bool loadStreamSamples(const std::string &path,
                           const StreamColumns &columns,
                           std::vector<AlignSample> &samplesOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }
        // ponytail: the whole recording lands in memory as samples (48 bytes
        // a row, against roughly 150 on disk). A session long enough for that
        // to hurt needs a streaming join, which the two-pointer walk allows.
        std::string line;
        bool header = true;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (header)
            {
                header = false;
                continue;
            }
            if (line.empty())
            {
                continue;
            }
            const std::vector<std::string> cells = csvCells(line);
            const std::size_t needed =
                std::max({columns.attitudeQuat + 3U, columns.altitude, columns.verticalVelocity});
            if (cells.size() <= needed)
            {
                continue;
            }
            AlignSample sample;
            sample.timestampUs = cellValue(cells[0]);
            for (std::size_t index = 0U; index < sample.attitudeQuat.size(); ++index)
            {
                sample.attitudeQuat[index] = cellValue(cells[columns.attitudeQuat + index]);
            }
            sample.altitudeM = cellValue(cells[columns.altitude]);
            sample.verticalVelocityMps = cellValue(cells[columns.verticalVelocity]);
            samplesOut.push_back(sample);
        }
        return true;
    }
} // namespace mark4
