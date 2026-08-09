/// @file
/// @brief Stream recorder implementation.

#include "hub/stream_recorder.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace mark4
{
    namespace
    {
        /// Room for the longest shortest-round-trip double plus its sign,
        /// its exponent and the terminator.
        constexpr std::size_t FLOAT_BUFFER_SIZE = 48U;

        /// Decimal point position above which python switches to the
        /// exponent form, and below which (negated) it does the same.
        constexpr int FIXED_UPPER_BOUND = 16;
        constexpr int FIXED_LOWER_BOUND = -4;

        /// Exponents are written with at least two digits, as printf does.
        constexpr int MIN_EXPONENT_DIGITS = 2;

        /// @brief Renders the significant digits and the decimal point
        ///        position of a finite value, shortest round trip.
        /// @param value value to decompose, neither zero-signed nor special
        /// @param digitsOut receives the significant digits, no dot, no sign
        /// @return decimal point position: the value is 0.<digits> * 10^result
        int shortestDigits(double value, std::string &digitsOut)
        {
            std::array<char, FLOAT_BUFFER_SIZE> buffer{};
            const std::to_chars_result written = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
            const std::string_view text(buffer.data(),
                                        static_cast<std::size_t>(written.ptr - buffer.data()));

            const std::size_t exponentAt = text.find('e');
            digitsOut.clear();
            for (const char character : text.substr(0U, exponentAt))
            {
                if (character != '.' && character != '-')
                {
                    digitsOut.push_back(character);
                }
            }
            int exponent = 0;
            std::string_view exponentText = text.substr(exponentAt + 1U);
            // from_chars refuses a leading plus sign, which to_chars writes.
            if (!exponentText.empty() && exponentText.front() == '+')
            {
                exponentText.remove_prefix(1U);
            }
            static_cast<void>(std::from_chars(
                exponentText.data(), exponentText.data() + exponentText.size(), exponent));
            return exponent + 1;
        }

        /// @brief Renders a value the way python's repr() renders a float.
        /// @param value value to render
        /// @return the decimal text
        std::string pythonRepr(double value)
        {
            if (std::isnan(value))
            {
                return "nan";
            }
            const std::string sign = std::signbit(value) ? "-" : "";
            if (std::isinf(value))
            {
                return sign + "inf";
            }

            std::string digits;
            const int decimalPoint = shortestDigits(value, digits);
            if (decimalPoint > FIXED_UPPER_BOUND || decimalPoint <= FIXED_LOWER_BOUND)
            {
                // Exponent form: one digit, the rest as a fraction, and an
                // exponent of at least two digits like printf writes it.
                std::string mantissa = digits.substr(0U, 1U);
                if (digits.size() > 1U)
                {
                    mantissa += "." + digits.substr(1U);
                }
                const int exponent = decimalPoint - 1;
                std::string exponentDigits = std::to_string(std::abs(exponent));
                while (exponentDigits.size() < static_cast<std::size_t>(MIN_EXPONENT_DIGITS))
                {
                    exponentDigits.insert(exponentDigits.begin(), '0');
                }
                return sign + mantissa + "e" + (exponent < 0 ? "-" : "+") + exponentDigits;
            }

            const auto point = static_cast<std::size_t>(decimalPoint);
            if (decimalPoint <= 0)
            {
                return sign + "0." + std::string(static_cast<std::size_t>(-decimalPoint), '0') +
                       digits;
            }
            if (point >= digits.size())
            {
                return sign + digits + std::string(point - digits.size(), '0') + ".0";
            }
            return sign + digits.substr(0U, point) + "." + digits.substr(point);
        }

        /// @brief Appends one CSV cell holding a float.
        /// @param row row being built
        /// @param value value to append
        void appendCell(std::string &row, float value)
        {
            row += ',';
            row += formatCsvFloat(value);
        }

        /// @brief Appends one CSV cell per component of a float array.
        /// @param row row being built
        /// @param values values to append
        template <std::size_t N>
        void appendCells(std::string &row, const std::array<float, N> &values)
        {
            for (const float value : values)
            {
                appendCell(row, value);
            }
        }
    } // namespace

    std::string formatCsvFloat(float value)
    {
        return pythonRepr(static_cast<double>(value));
    }

    StreamRecorder::StreamRecorder(std::string logDirectory)
        : m_logDirectory(std::move(logDirectory))
    {
    }

    std::string StreamRecorder::stampedPath(const char *pattern) const
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        static_cast<void>(localtime_r(&now, &local));
        std::array<char, PATH_SIZE> name{};
        const std::size_t written = std::strftime(name.data(), name.size(), pattern, &local);
        return m_logDirectory + "/" + std::string(name.data(), written);
    }

    bool StreamRecorder::startCsvSession()
    {
        stopCsvSession();

        std::error_code failure;
        std::filesystem::create_directories(m_logDirectory, failure);

        m_telemetryCsvPath = stampedPath("streams_%Y%m%d_%H%M%S_telemetry.csv");
        m_simRawCsvPath = stampedPath("streams_%Y%m%d_%H%M%S_simraw.csv");
        // Binary mode: the CRLF terminator is written explicitly, it must not
        // be translated a second time by a text-mode stream.
        m_telemetryCsv.open(m_telemetryCsvPath, std::ios::binary | std::ios::trunc);
        m_simRawCsv.open(m_simRawCsvPath, std::ios::binary | std::ios::trunc);
        if (!csvSessionOpen())
        {
            stopCsvSession();
            return false;
        }
        m_telemetryCsv << TELEMETRY_CSV_HEADER << CSV_LINE_END;
        m_simRawCsv << SIM_RAW_CSV_HEADER << CSV_LINE_END;
        return true;
    }

    void StreamRecorder::stopCsvSession()
    {
        if (m_telemetryCsv.is_open())
        {
            m_telemetryCsv.close();
        }
        if (m_simRawCsv.is_open())
        {
            m_simRawCsv.close();
        }
    }

    void StreamRecorder::onTelemetry(const TelemetryPacket &packet)
    {
        if (!csvSessionOpen())
        {
            return;
        }
        std::string row = std::to_string(packet.timestampUs);
        appendCells(row, packet.gyroRadS);
        appendCells(row, packet.attitudeQuat);
        appendCells(row, packet.gyroBiasRadS);
        appendCells(row, packet.motor);
        appendCell(row, packet.altitudeM);
        appendCell(row, packet.verticalVelocityMps);
        m_telemetryCsv << row << CSV_LINE_END;
        ++m_stats.telemetryRows;
    }

    void StreamRecorder::onSimRaw(const SimRawPacket &packet)
    {
        if (!csvSessionOpen())
        {
            return;
        }
        std::string row = std::to_string(packet.timestampUs);
        appendCells(row, packet.attitudeQuat);
        appendCells(row, packet.positionM);
        appendCells(row, packet.velocityMps);
        m_simRawCsv << row << CSV_LINE_END;
        ++m_stats.simRawRows;
    }

    bool StreamRecorder::onBlackboxRecord(const std::uint8_t *record, std::size_t size)
    {
        if (!m_blackbox.is_open())
        {
            std::error_code failure;
            std::filesystem::create_directories(m_logDirectory, failure);
            m_blackboxPath = stampedPath("board_%Y%m%d_%H%M%S.m4bb");
            m_blackbox.open(m_blackboxPath, std::ios::binary | std::ios::trunc);
            if (!m_blackbox.is_open())
            {
                m_blackboxPath.clear();
                return false;
            }
        }
        m_blackbox.write(reinterpret_cast<const char *>(record),
                         static_cast<std::streamsize>(size));
        m_blackbox.flush();
        ++m_stats.blackboxRecords;
        return m_blackbox.good();
    }
} // namespace mark4
