/// @file
/// @brief HTTP routing implementation.

#include "hub/http_api.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

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
        const std::size_t query = uri.find('?');
        const std::string_view path =
            (query == std::string_view::npos) ? uri : uri.substr(0U, query);
        return servePage(config, path);
    }
} // namespace mark4
