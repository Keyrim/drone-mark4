/// @file
/// @brief HTTP routing implementation.

#include "hub/http_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "hub/recordings.hpp"

namespace mark4
{
    namespace
    {
        /// One extension and the MIME type it maps to. Javascript is
        /// text/javascript and nothing else: a browser silently refuses to
        /// run an ES module served under any other type.
        struct MimeEntry
        {
            const char *extension; ///< extension, dot included
            const char *type;      ///< MIME type to answer with
        };

        constexpr MimeEntry MIME_TABLE[] = {
            {".html", "text/html; charset=utf-8"},
            {".js", "text/javascript; charset=utf-8"},
            {".mjs", "text/javascript; charset=utf-8"},
            {".css", "text/css; charset=utf-8"},
            {".svg", "image/svg+xml"},
            {".json", "application/json"},
            {".csv", "text/csv; charset=utf-8"},
            {".ico", "image/x-icon"},
            {".png", "image/png"},
        };

        /// @brief Turns a request target into a path relative to the pages
        ///        directory. Everything that could climb out of it is
        ///        refused rather than normalized: there is no legitimate
        ///        page whose URI needs a "..".
        /// @param path request target, query string already removed
        /// @param relativeOut receives the relative path
        /// @return true when the target stays inside the pages directory
        bool safeRelativePath(std::string_view path, std::string &relativeOut)
        {
            std::string relative;
            std::size_t index = 0U;
            while (index < path.size())
            {
                const std::size_t end = path.find('/', index);
                const std::size_t length =
                    (end == std::string_view::npos) ? std::string_view::npos : end - index;
                const std::string_view part = path.substr(index, length);
                index = (end == std::string_view::npos) ? path.size() : end + 1U;
                if (part.empty() || part == ".")
                {
                    continue;
                }
                if (part == "..")
                {
                    return false;
                }
                if (!relative.empty())
                {
                    relative += '/';
                }
                relative.append(part);
            }
            // Both "" and "/" are the index page; a directory below it is not
            // browsable, only its own index would be, and that is a URI a
            // page spells out.
            relativeOut = relative.empty() ? std::string("index.html") : relative;
            return true;
        }

        /// @brief Serves one file below the pages directory.
        /// @param config where to read from
        /// @param path request target, query string already removed
        /// @return the file, or a 404
        HttpResult servePage(const HttpConfig &config, std::string_view path)
        {
            HttpResult result;
            result.status = HTTP_NOT_FOUND;
            result.contentType = "text/plain; charset=utf-8";
            result.body = "not found";

            std::string relative;
            if (config.pagesDir.empty() || !safeRelativePath(path, relative))
            {
                return result;
            }

            const std::filesystem::path file =
                std::filesystem::path(config.pagesDir) / std::filesystem::path(relative);
            std::error_code failure;
            if (!std::filesystem::is_regular_file(file, failure))
            {
                return result;
            }
            std::ifstream stream(file, std::ios::binary);
            if (!stream.is_open())
            {
                return result;
            }
            result.status = HTTP_OK;
            result.contentType = mimeTypeOf(relative);
            result.body.assign(std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>());
            return result;
        }

        /// The parameters of one request, decoded.
        using Query = std::vector<std::pair<std::string, std::string>>;

        /// Value the letters of a hexadecimal digit start at.
        constexpr int HEX_LETTER_BASE = 10;

        /// @brief Value of one hexadecimal digit.
        /// @param digit character to read
        /// @return its value, -1 when it is not one
        int hexDigit(char digit)
        {
            if (digit >= '0' && digit <= '9')
            {
                return digit - '0';
            }
            if (digit >= 'a' && digit <= 'f')
            {
                return digit - 'a' + HEX_LETTER_BASE;
            }
            if (digit >= 'A' && digit <= 'F')
            {
                return digit - 'A' + HEX_LETTER_BASE;
            }
            return -1;
        }

        /// @brief Undoes the percent encoding of a query value.
        /// @param text text to decode
        /// @return the decoded text; an incomplete escape is kept as written
        std::string percentDecoded(std::string_view text)
        {
            static constexpr int HEX_SHIFT = 4;
            std::string decoded;
            decoded.reserve(text.size());
            for (std::size_t index = 0U; index < text.size(); ++index)
            {
                if (text[index] == '+')
                {
                    decoded += ' ';
                    continue;
                }
                if (text[index] != '%' || index + 2U >= text.size())
                {
                    decoded += text[index];
                    continue;
                }
                const int high = hexDigit(text[index + 1U]);
                const int low = hexDigit(text[index + 2U]);
                if (high < 0 || low < 0)
                {
                    decoded += text[index];
                    continue;
                }
                decoded += static_cast<char>((high << HEX_SHIFT) | low);
                index += 2U;
            }
            return decoded;
        }

        /// @brief Splits the query string of a request target.
        /// @param uri request target
        /// @return the parameters, in the order they were written
        Query parseQuery(std::string_view uri)
        {
            Query query;
            const std::size_t mark = uri.find('?');
            if (mark == std::string_view::npos)
            {
                return query;
            }
            std::string_view rest = uri.substr(mark + 1U);
            while (!rest.empty())
            {
                const std::size_t end = rest.find('&');
                const std::string_view item = rest.substr(0U, end);
                rest = (end == std::string_view::npos) ? std::string_view() : rest.substr(end + 1U);
                if (item.empty())
                {
                    continue;
                }
                const std::size_t equals = item.find('=');
                if (equals == std::string_view::npos)
                {
                    query.emplace_back(percentDecoded(item), std::string());
                    continue;
                }
                query.emplace_back(percentDecoded(item.substr(0U, equals)),
                                   percentDecoded(item.substr(equals + 1U)));
            }
            return query;
        }

        /// @brief Value of one parameter.
        /// @param query parameters of the request
        /// @param key parameter to look for
        /// @return its value, empty when it was not sent
        std::string queryValue(const Query &query, const char *key)
        {
            for (const auto &item : query)
            {
                if (item.first == key)
                {
                    return item.second;
                }
            }
            return {};
        }

        /// @brief One answer carrying a reason instead of a result.
        /// @param status status code to answer with
        /// @param reason what went wrong
        /// @return the answer
        HttpResult jsonError(int status, const std::string &reason)
        {
            HubJson body;
            body["error"] = reason;
            HttpResult result;
            result.status = status;
            result.contentType = "application/json";
            result.body = body.dump();
            return result;
        }

        /// @brief One answer carrying JSON.
        /// @param body body to answer with
        /// @return the answer
        HttpResult jsonAnswer(const HubJson &body)
        {
            HttpResult result;
            result.contentType = "application/json";
            result.body = body.dump();
            return result;
        }

        /// @brief Reads the window parameters of a request.
        /// @param query parameters of the request
        /// @param windowOut receives the window
        /// @param errorOut receives the reason when a parameter is unusable
        /// @return true when the parameters make a window
        bool readWindow(const Query &query, SampleWindow &windowOut, std::string &errorOut)
        {
            static constexpr int BASE = 10;
            const std::string from = queryValue(query, "from");
            const std::string to = queryValue(query, "to");
            const std::string maxPoints = queryValue(query, "maxPoints");
            if (!from.empty())
            {
                windowOut.fromUs = std::strtoull(from.c_str(), nullptr, BASE);
            }
            if (!to.empty())
            {
                windowOut.toUs = std::strtoull(to.c_str(), nullptr, BASE);
            }
            if (!maxPoints.empty())
            {
                const auto wanted = std::strtoull(maxPoints.c_str(), nullptr, BASE);
                if (wanted == 0U)
                {
                    errorOut = "maxPoints must be at least 1";
                    return false;
                }
                windowOut.maxPoints = std::min<std::size_t>(wanted, POINT_LIMIT);
            }
            if (windowOut.fromUs > windowOut.toUs)
            {
                errorOut = "from must not be after to";
                return false;
            }
            return true;
        }

        /// @brief Looks one recording up by the name a caller sent.
        /// @param config where to read from
        /// @param query parameters of the request
        /// @param recordingOut receives the recording
        /// @param errorOut receives the answer to send when there is none
        /// @return true when the listing holds that name
        bool lookUpRecording(const HttpConfig &config,
                             const Query &query,
                             Recording &recordingOut,
                             HttpResult &errorOut)
        {
            const std::string name = queryValue(query, "name");
            if (name.empty())
            {
                errorOut = jsonError(HTTP_BAD_REQUEST, "name is required");
                return false;
            }
            // The listing is the whole address space: a name it does not hold
            // addresses nothing, so nothing a caller sends ever becomes a
            // path of its own.
            if (!findRecording(listRecordings(config.logDir), name, recordingOut))
            {
                errorOut = jsonError(HTTP_NOT_FOUND, "no recording named " + name);
                return false;
            }
            return true;
        }

        /// @brief Answers GET /api/recordings.
        /// @param config where to read from
        /// @return the answer
        HttpResult apiRecordings(const HttpConfig &config)
        {
            return jsonAnswer(recordingsToJson(config.logDir, listRecordings(config.logDir)));
        }

        /// @brief Answers GET /api/recording.
        /// @param config where to read from
        /// @param query parameters of the request
        /// @return the answer
        HttpResult apiRecording(const HttpConfig &config, const Query &query)
        {
            Recording recording;
            HttpResult failure;
            if (!lookUpRecording(config, query, recording, failure))
            {
                return failure;
            }
            SampleWindow window;
            std::string reason;
            if (!readWindow(query, window, reason))
            {
                return jsonError(HTTP_BAD_REQUEST, reason);
            }
            const HubJson decoded = decodeRecording(config.logDir, recording, window);
            if (decoded.is_null())
            {
                return jsonError(HTTP_NOT_FOUND, "cannot read " + recording.name);
            }
            return jsonAnswer(decoded);
        }

        /// @brief Answers GET /api/compare.
        /// @param config where to read from
        /// @param query parameters of the request
        /// @return the answer
        HttpResult apiCompare(const HttpConfig &config, const Query &query)
        {
            Recording recording;
            HttpResult failure;
            if (!lookUpRecording(config, query, recording, failure))
            {
                return failure;
            }
            if (recording.kind != "streams" || recording.simRawFile.empty())
            {
                // There is nothing to compare an estimate against without the
                // exact state recorded beside it.
                return jsonError(HTTP_BAD_REQUEST,
                                 recording.name + " has no exact state to compare against");
            }
            SampleWindow window;
            std::string reason;
            if (!readWindow(query, window, reason))
            {
                return jsonError(HTTP_BAD_REQUEST, reason);
            }
            const HubJson scored = compareRecording(config.logDir, recording, window);
            if (scored.is_null())
            {
                return jsonError(HTTP_NOT_FOUND, "cannot read " + recording.name);
            }
            return jsonAnswer(scored);
        }

        /// @brief Answers GET /api/summary.
        /// @param config where to read from
        /// @param query parameters of the request
        /// @return the answer
        HttpResult apiSummary(const HttpConfig &config, const Query &query)
        {
            Recording recording;
            HttpResult failure;
            if (!lookUpRecording(config, query, recording, failure))
            {
                return failure;
            }
            if (recording.kind != "blackbox")
            {
                return jsonError(HTTP_BAD_REQUEST, recording.name + " is not a blackbox recording");
            }
            const HubJson summary = summarizeBlackbox(config.logDir, recording);
            if (summary.is_null())
            {
                return jsonError(HTTP_NOT_FOUND, "cannot read " + recording.name);
            }
            return jsonAnswer(summary);
        }

        /// @brief Answers GET /api/file.
        /// @param config where to read from
        /// @param query parameters of the request
        /// @return the answer, the file itself as a download
        HttpResult apiFile(const HttpConfig &config, const Query &query)
        {
            Recording recording;
            HttpResult failure;
            if (!lookUpRecording(config, query, recording, failure))
            {
                return failure;
            }
            std::string part = queryValue(query, "part");
            if (part.empty())
            {
                part = (recording.kind == "blackbox") ? "raw" : "telemetry";
            }

            std::string fileName;
            if (recording.kind == "blackbox")
            {
                if (part == "csv")
                {
                    HttpResult result;
                    result.contentType = "text/csv; charset=utf-8";
                    result.body = blackboxToCsv(
                        (std::filesystem::path(recordingDirectory(config.logDir, recording)) /
                         recording.name)
                            .string());
                    result.attachmentName = recording.name + ".csv";
                    if (result.body.empty())
                    {
                        return jsonError(HTTP_NOT_FOUND, "cannot read " + recording.name);
                    }
                    return result;
                }
                if (part != "raw")
                {
                    return jsonError(HTTP_BAD_REQUEST, "no part named " + part);
                }
                fileName = recording.name;
            }
            else if (part == "telemetry")
            {
                fileName = recording.telemetryFile;
            }
            else if (part == "simraw")
            {
                fileName = recording.simRawFile;
            }
            else
            {
                return jsonError(HTTP_BAD_REQUEST, "no part named " + part);
            }

            HttpResult result;
            // ponytail: the file is read whole because the http library takes
            // a body and not a stream. A recording large enough to matter
            // wants a chunked response instead.
            if (fileName.empty() ||
                !readWholeFile(
                    (std::filesystem::path(recordingDirectory(config.logDir, recording)) / fileName)
                        .string(),
                    result.body))
            {
                return jsonError(HTTP_NOT_FOUND, "cannot read " + part + " of " + recording.name);
            }
            result.contentType = mimeTypeOf(fileName);
            result.attachmentName = fileName;
            return result;
        }
    } // namespace

    std::string mimeTypeOf(std::string_view path)
    {
        const std::size_t dot = path.rfind('.');
        if (dot != std::string_view::npos)
        {
            const std::string_view extension = path.substr(dot);
            for (const MimeEntry &entry : MIME_TABLE)
            {
                if (extension == entry.extension)
                {
                    return entry.type;
                }
            }
        }
        return "application/octet-stream";
    }

    HttpResult routeHttp(const HttpConfig &config, std::string_view method, std::string_view uri)
    {
        if (method != "GET" && method != "HEAD")
        {
            HttpResult result;
            result.status = HTTP_METHOD_NOT_ALLOWED;
            result.contentType = "text/plain; charset=utf-8";
            result.body = "method not allowed";
            return result;
        }
        const std::size_t mark = uri.find('?');
        const std::string_view path = (mark == std::string_view::npos) ? uri : uri.substr(0U, mark);
        if (path == "/api/recordings")
        {
            return apiRecordings(config);
        }
        if (path == "/api/recording")
        {
            return apiRecording(config, parseQuery(uri));
        }
        if (path == "/api/compare")
        {
            return apiCompare(config, parseQuery(uri));
        }
        if (path == "/api/summary")
        {
            return apiSummary(config, parseQuery(uri));
        }
        if (path == "/api/file")
        {
            return apiFile(config, parseQuery(uri));
        }
        if (path.rfind("/api/", 0U) == 0U)
        {
            return jsonError(HTTP_NOT_FOUND, "no such endpoint");
        }
        return servePage(config, path);
    }
} // namespace mark4
