#pragma once

/// @file
/// @brief Reading back what the hub wrote: the log directory as a list, and
///        the CSV pairs and .m4bb files in it as decoded points a page can
///        draw.
///
///        Files only. Nothing here looks at a socket, a counter or the
///        recorder: a recording is finished business, and that is what makes
///        it safe to answer from any thread.
///
///        A decode never holds a whole file in memory: it walks the file
///        once to count what falls in the asked-for window, then a second
///        time to emit every n-th point of it. A blackbox file is tens of
///        megabytes and a browser wants two thousand points.

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mark4
{
    /// Insertion-ordered JSON, so an answer reads in the order it was built.
    using HubJson = nlohmann::ordered_json;

    /// Points a decode answers with when the caller asks for no number.
    inline constexpr std::size_t DEFAULT_MAX_POINTS = 2000U;

    /// Most points a decode will answer with, whatever is asked for. Past
    /// this the answer stops being a curve and becomes a download.
    inline constexpr std::size_t POINT_LIMIT = 20000U;

    /// Subdirectory of the log directory the blackbox files live in.
    inline constexpr const char *BLACKBOX_SUBDIR = "blackbox";

    /// Subdirectory of the log directory the stream CSV pairs live in.
    inline constexpr const char *STREAMS_SUBDIR = "streams";

    /// One recording found in the log directory.
    struct Recording
    {
        std::string name;                    ///< how everything else addresses it
        std::string kind;                    ///< "blackbox" or "streams"
        std::uint64_t sizeBytes = 0U;        ///< bytes on disk, both files for a pair
        std::int64_t modifiedUnixS = 0;      ///< last write, seconds since the epoch
        std::uint64_t estimatedRecords = 0U; ///< blackbox: size divided by the record size
        std::string telemetryFile;           ///< streams: telemetry CSV file name
        std::string simRawFile;              ///< streams: sim raw CSV file name, empty when absent
    };

    /// Which part of a recording an answer covers, and how finely.
    struct SampleWindow
    {
        std::uint64_t fromUs = 0U;                  ///< first instant to include [us]
        std::uint64_t toUs = UINT64_MAX;            ///< last instant to include [us]
        std::size_t maxPoints = DEFAULT_MAX_POINTS; ///< points to answer with at most
    };

    /// @brief Directory the files of one recording live in: the log
    ///        directory subdirectory its kind names.
    /// @param logDir root log directory
    /// @param recording recording to locate
    /// @return logDir/blackbox or logDir/streams
    std::string recordingDirectory(const std::string &logDir, const Recording &recording);

    /// @brief Lists what the log directory holds, most recent first. The
    ///        blackbox files live in blackbox/, the stream CSV pairs in
    ///        streams/; a telemetry CSV and the sim raw CSV recorded beside
    ///        it are one recording, named by the prefix they share.
    /// @param logDir directory to list
    /// @return the recordings
    std::vector<Recording> listRecordings(const std::string &logDir);

    /// @brief Finds one recording by the exact name the listing gave it.
    ///        This is the whole address space of the API: a name that is not
    ///        in the listing addresses nothing, so no path can be built out
    ///        of what a caller sent.
    /// @param recordings the listing
    /// @param name name to look for
    /// @param recordingOut receives the recording when found
    /// @return true when the listing holds it
    bool findRecording(const std::vector<Recording> &recordings,
                       const std::string &name,
                       Recording &recordingOut);

    /// @brief Renders the listing.
    /// @param logDir directory the recordings live in
    /// @param recordings the listing
    /// @return the JSON answer
    HubJson recordingsToJson(const std::string &logDir, const std::vector<Recording> &recordings);

    /// @brief Decodes one recording into points, decimated to the window.
    /// @param logDir directory the recording lives in
    /// @param recording recording to decode
    /// @param window part of it to cover, and how finely
    /// @return the JSON answer
    HubJson decodeRecording(const std::string &logDir,
                            const Recording &recording,
                            const SampleWindow &window);

    /// @brief Scores one streams recording: the same alignment and the same
    ///        scoring the live comparison runs, on the recorded pair.
    /// @param logDir directory the recording lives in
    /// @param recording recording to score, of kind "streams"
    /// @param window part of it to cover, and how finely
    /// @return the JSON answer
    HubJson compareRecording(const std::string &logDir,
                             const Recording &recording,
                             const SampleWindow &window);

    /// @brief Summarizes one blackbox recording: how long, how fast, how
    ///        hard it was shaken, how much of it was torn.
    /// @param logDir directory the recording lives in
    /// @param recording recording to summarize, of kind "blackbox"
    /// @return the JSON answer
    HubJson summarizeBlackbox(const std::string &logDir, const Recording &recording);

    /// @brief Header line of the CSV rendering of a blackbox file.
    /// @return the header
    const char *blackboxCsvHeader();

    /// @brief Renders a whole blackbox file as CSV, one line per record.
    /// @param path file to render
    /// @return the CSV text, empty when the file cannot be read
    std::string blackboxToCsv(const std::string &path);

    /// @brief Reads a whole file.
    /// @param path file to read
    /// @param contentOut receives the bytes
    /// @return true when the file could be read
    bool readWholeFile(const std::string &path, std::string &contentOut);
} // namespace mark4
