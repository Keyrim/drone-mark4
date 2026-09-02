/// @file
/// @brief HTTP routing implementation.

#include "hub/http_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

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

        /// One family of stored files under the telemetry directory. The
        /// three differ only in where they live, what suffix they carry and
        /// whether their body has to be JSON, so they share one handler.
        struct Collection
        {
            const char *segment;   ///< first path segment under /api/telemetry/
            const char *directory; ///< subdirectory of the telemetry directory
            const char *suffix;    ///< suffix every file of the family carries
            const char *type;      ///< MIME type a GET answers with
            bool json;             ///< a PUT body must parse as JSON
            bool download;         ///< a GET offers the file as a download
        };

        /// Sessions are whole recordings, exports are what a spreadsheet
        /// opens, configs are the ticked series with no data in them.
        constexpr Collection COLLECTIONS[] = {
            {"sessions", "sessions", ".telemetry.json", "application/json", true, false},
            {"exports", "exports", ".csv", "text/csv; charset=utf-8", false, true},
            {"configs", "configs", ".json", "application/json", true, false},
        };

        /// @param status status code
        /// @param body JSON body
        /// @return one JSON answer
        HttpResult jsonResult(int status, std::string body)
        {
            HttpResult result;
            result.status = status;
            result.contentType = "application/json";
            result.body = std::move(body);
            return result;
        }

        /// @param reason what the client got wrong, one sentence
        /// @return one 400
        HttpResult badRequest(const std::string &reason)
        {
            return jsonResult(HTTP_BAD_REQUEST, nlohmann::json{{"error", reason}}.dump());
        }

        /// @return one 404 for an API path
        HttpResult apiNotFound()
        {
            return jsonResult(HTTP_NOT_FOUND, R"({"error":"no such endpoint"})");
        }

        /// @return one 405
        HttpResult methodNotAllowed()
        {
            return jsonResult(HTTP_METHOD_NOT_ALLOWED, R"({"error":"method not allowed"})");
        }

        /// @param name file name of a stored file
        /// @param collection family it belongs to
        /// @return the stored name, empty when the suffix does not match
        std::string storedNameOf(const std::string &name, const Collection &collection)
        {
            const std::string suffix = collection.suffix;
            if (name.size() <= suffix.size() ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            {
                return {};
            }
            return name.substr(0U, name.size() - suffix.size());
        }

        /// @brief Lists one family, newest first is the client's business:
        ///        the answer is sorted by name so two listings of the same
        ///        directory read the same.
        /// @param directory directory to list
        /// @param collection family it holds
        /// @return the JSON array
        HttpResult listCollection(const std::filesystem::path &directory,
                                  const Collection &collection)
        {
            nlohmann::json entries = nlohmann::json::array();
            std::error_code failure;
            // The non-throwing overloads throughout: a directory nothing has
            // been saved into yet is an empty list, not an error.
            std::vector<std::filesystem::directory_entry> found;
            for (const auto &entry : std::filesystem::directory_iterator(directory, failure))
            {
                found.push_back(entry);
            }
            std::sort(found.begin(), found.end(), [](const auto &left, const auto &right) {
                return left.path().filename() < right.path().filename();
            });
            for (const auto &entry : found)
            {
                const std::string name = storedNameOf(entry.path().filename().string(), collection);
                if (name.empty() || !validStoredName(name))
                {
                    continue;
                }
                std::error_code sizeFailure;
                const std::uintmax_t bytes = std::filesystem::file_size(entry.path(), sizeFailure);
                std::error_code timeFailure;
                const auto written = std::filesystem::last_write_time(entry.path(), timeFailure);
                // The epoch of a file clock is whatever the implementation
                // chose, so it has to be converted before it means anything
                // to a client: unix seconds is what a page formats.
                const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::file_clock::to_sys(written).time_since_epoch())
                                       .count();
                entries.push_back({{"name", name},
                                   {"bytes", sizeFailure ? 0U : bytes},
                                   {"modified", timeFailure ? 0 : epoch}});
            }
            return jsonResult(HTTP_OK, entries.dump());
        }

        /// @brief Serves one stored file.
        /// @param file path to read
        /// @param name stored name, for the download file name
        /// @param collection family it belongs to
        /// @return the file, or a 404
        HttpResult readStored(const std::filesystem::path &file,
                              const std::string &name,
                              const Collection &collection)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream.is_open())
            {
                return jsonResult(HTTP_NOT_FOUND, R"({"error":"no such file"})");
            }
            HttpResult result;
            result.contentType = collection.type;
            result.body.assign(std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>());
            if (collection.download)
            {
                result.attachmentName = name + collection.suffix;
            }
            return result;
        }

        /// @brief Writes one stored file, creating its directory.
        /// @param directory directory to write into
        /// @param file path to write
        /// @param name stored name, echoed in the answer
        /// @param collection family it belongs to
        /// @param body what to store
        /// @return 200 with the name and the size, or the refusal
        HttpResult writeStored(const std::filesystem::path &directory,
                               const std::filesystem::path &file,
                               const std::string &name,
                               const Collection &collection,
                               std::string_view body)
        {
            if (body.empty())
            {
                return badRequest("empty body");
            }
            if (body.size() > HTTP_MAX_BODY)
            {
                return badRequest("body over " + std::to_string(HTTP_MAX_BODY) + " bytes");
            }
            if (collection.json)
            {
                // Parsed but not interpreted: the shape of a session or a
                // config is the page's business, and the hub only refuses to
                // store what it could never hand back as JSON.
                const auto parsed = nlohmann::json::parse(body, nullptr, false);
                if (parsed.is_discarded())
                {
                    return badRequest("the body is not JSON");
                }
            }
            std::error_code failure;
            std::filesystem::create_directories(directory, failure);
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            if (!stream.is_open())
            {
                return badRequest("cannot write " + file.string());
            }
            stream.write(body.data(), static_cast<std::streamsize>(body.size()));
            stream.close();
            if (!stream)
            {
                return badRequest("cannot write " + file.string());
            }
            return jsonResult(HTTP_OK,
                              nlohmann::json{{"name", name}, {"bytes", body.size()}}.dump());
        }

        /// @brief Answers one request under /api/telemetry/.
        /// @param config where to read and write
        /// @param method HTTP method
        /// @param path request target, query string already removed
        /// @param body request body
        /// @return the answer
        HttpResult routeTelemetry(const HttpConfig &config,
                                  std::string_view method,
                                  std::string_view path,
                                  std::string_view body)
        {
            if (config.telemetryDir.empty())
            {
                return apiNotFound();
            }
            const std::size_t slash = path.find('/');
            const std::string_view segment = path.substr(0U, slash);
            const Collection *collection = nullptr;
            for (const Collection &candidate : COLLECTIONS)
            {
                if (segment == candidate.segment)
                {
                    collection = &candidate;
                }
            }
            if (collection == nullptr)
            {
                return apiNotFound();
            }

            const std::filesystem::path directory =
                std::filesystem::path(config.telemetryDir) / collection->directory;
            if (slash == std::string_view::npos)
            {
                // The collection itself: only a listing.
                return method == "GET" || method == "HEAD" ? listCollection(directory, *collection)
                                                           : methodNotAllowed();
            }

            // The suffix is part of the URI for the exports (a browser
            // downloading one wants the .csv in the link) and not for the
            // rest, so it is stripped when it is there and required nowhere.
            std::string name(path.substr(slash + 1U));
            const std::string stripped = storedNameOf(name, *collection);
            if (!stripped.empty())
            {
                name = stripped;
            }
            if (name.find('/') != std::string::npos)
            {
                return apiNotFound();
            }
            if (!validStoredName(name))
            {
                return badRequest("a name is 1 to " + std::to_string(HTTP_MAX_NAME) +
                                  " characters of letters, digits, '_' and '-'");
            }
            const std::filesystem::path file = directory / (name + collection->suffix);

            if (method == "GET" || method == "HEAD")
            {
                return readStored(file, name, *collection);
            }
            if (method == "PUT")
            {
                return writeStored(directory, file, name, *collection, body);
            }
            if (method == "DELETE")
            {
                std::error_code failure;
                if (!std::filesystem::remove(file, failure) || failure)
                {
                    return jsonResult(HTTP_NOT_FOUND, R"({"error":"no such file"})");
                }
                return jsonResult(HTTP_OK, nlohmann::json{{"name", name}}.dump());
            }
            return methodNotAllowed();
        }
    } // namespace

    bool validStoredName(std::string_view name)
    {
        if (name.empty() || name.size() > HTTP_MAX_NAME)
        {
            return false;
        }
        return std::all_of(name.begin(), name.end(), [](char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') || character == '_' || character == '-';
        });
    }

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

    HttpResult routeHttp(const HttpConfig &config,
                         std::string_view method,
                         std::string_view uri,
                         std::string_view body)
    {
        const std::size_t mark = uri.find('?');
        const std::string_view path = (mark == std::string_view::npos) ? uri : uri.substr(0U, mark);
        static constexpr std::string_view TELEMETRY_PREFIX = "/api/telemetry/";
        if (path.rfind(TELEMETRY_PREFIX, 0U) == 0U)
        {
            return routeTelemetry(config, method, path.substr(TELEMETRY_PREFIX.size()), body);
        }
        if (path == "/api/telemetry")
        {
            return apiNotFound();
        }
        if (method != "GET" && method != "HEAD")
        {
            HttpResult result;
            result.status = HTTP_METHOD_NOT_ALLOWED;
            result.contentType = "text/plain; charset=utf-8";
            result.body = "method not allowed";
            return result;
        }
        if (path.rfind("/api/", 0U) == 0U)
        {
            return apiNotFound();
        }
        return servePage(config, path);
    }
} // namespace mark4
