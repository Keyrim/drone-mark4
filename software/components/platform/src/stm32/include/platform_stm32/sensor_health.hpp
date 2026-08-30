#pragma once

/// @file
/// @brief Read-outcome tracker of one sensor: remembers when the last
///        good sample landed and logs the transitions, never the ticks.

#include <cstdint>

#include "log/module.hpp"

namespace mark4
{
    /// Fed one outcome per read attempt. Logs through the driver's own
    /// module, on transitions only: WARN on the first failure after a good
    /// run, ERROR once the failures lasted the horizon and again every
    /// ERROR_REPEAT_US while they last, INFO on the first success after
    /// failures. freshWithin() is what a frame's validity flag reads.
    class SensorHealth
    {
      public:
        /// Interval between two ERROR lines of one lasting outage [us].
        static constexpr std::uint64_t ERROR_REPEAT_US = 5000000U;

        /// @param module the driver's log module
        /// @param horizonUs failure duration that turns the WARN into an
        ///        ERROR [us]
        SensorHealth(LogModule &module, std::uint64_t horizonUs)
            : m_module(module),
              m_horizonUs(horizonUs)
        {
        }

        /// @brief Records one read attempt.
        /// @param ok true when a fresh sample was read
        /// @param nowUs instant of the attempt [us]
        void note(bool ok, std::uint64_t nowUs)
        {
            if (ok)
            {
                if (m_failedRun != 0U)
                {
                    m_module.info("recovered after %lu failed reads",
                                  static_cast<unsigned long>(m_failedRun));
                }
                m_failedRun = 0U;
                m_lastOkUs = nowUs;
                m_everOk = true;
                return;
            }
            if (m_failedRun == 0U)
            {
                m_module.warn("read failed, sample marked invalid");
                m_failStartUs = nowUs;
                m_errorLogged = false;
            }
            ++m_failedRun;
            const std::uint64_t outageUs = nowUs - m_failStartUs;
            if (outageUs >= m_horizonUs &&
                (!m_errorLogged || nowUs - m_lastErrorUs >= ERROR_REPEAT_US))
            {
                m_module.error("no sample for %lu ms",
                               static_cast<unsigned long>(outageUs / US_PER_MS));
                m_errorLogged = true;
                m_lastErrorUs = nowUs;
            }
        }

        /// @param nowUs current instant [us]
        /// @param maxAgeUs oldest age a sample may have to count [us]
        /// @return true when a good sample landed no more than maxAgeUs ago
        [[nodiscard]] bool freshWithin(std::uint64_t nowUs, std::uint64_t maxAgeUs) const
        {
            return m_everOk && nowUs - m_lastOkUs <= maxAgeUs;
        }

        /// @return consecutive failed reads, 0 after a success
        [[nodiscard]] std::uint32_t failedRun() const
        {
            return m_failedRun;
        }

      private:
        static constexpr std::uint64_t US_PER_MS = 1000U;

        LogModule &m_module;              ///< the driver's module, not owned
        std::uint64_t m_horizonUs;        ///< WARN to ERROR threshold [us]
        std::uint32_t m_failedRun = 0U;   ///< consecutive failures
        std::uint64_t m_failStartUs = 0U; ///< first failure of the run [us]
        std::uint64_t m_lastOkUs = 0U;    ///< last good sample [us]
        std::uint64_t m_lastErrorUs = 0U; ///< last ERROR line [us]
        bool m_everOk = false;            ///< a good sample was ever read
        bool m_errorLogged = false;       ///< the run already escalated
    };
} // namespace mark4
