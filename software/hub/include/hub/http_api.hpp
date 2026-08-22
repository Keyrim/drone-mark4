#pragma once

/// @file
/// @brief HTTP side of the hub endpoint: the static pages the browser loads,
///        served on the same TCP port as the websocket so a page never has to
///        know a port of its own.
///
///        Pure routing: a method and a URI go in, a response comes out. No
///        socket, no state, so a unit test drives the whole surface.
///
///        THE INVARIANT: /api/ is filesystem-only; everything live is
///        websocket. These handlers run on the connection threads of the
///        websocket library, so they must never touch the recorder, the
///        discovery registry or the counters. That is what keeps the hub
///        free of locks.

#include <string>
#include <string_view>

namespace mark4
{
    /// HTTP status codes the hub answers with.
    inline constexpr int HTTP_OK = 200;
    inline constexpr int HTTP_BAD_REQUEST = 400;
    inline constexpr int HTTP_NOT_FOUND = 404;
    inline constexpr int HTTP_METHOD_NOT_ALLOWED = 405;

    /// Where the HTTP side reads from. Filled once before the endpoint starts
    /// serving and never written again: the connection threads only read it.
    struct HttpConfig
    {
        std::string pagesDir; ///< directory the static pages are read from,
                              ///< empty or missing = every page is a 404
        std::string logDir;   ///< directory the recordings live in
    };

    /// One answer, ready to be turned into an HTTP response.
    struct HttpResult
    {
        int status = HTTP_OK;       ///< HTTP status code
        std::string contentType;    ///< value of the Content-Type header
        std::string body;           ///< response body
        std::string attachmentName; ///< file name to offer as a download,
                                    ///< empty when the body is not one
    };

    /// @brief MIME type a file name maps to, by extension. An extension the
    ///        table does not know is served as an opaque byte stream.
    /// @param path file name or path to type
    /// @return the MIME type
    std::string mimeTypeOf(std::string_view path);

    /// @brief Answers one HTTP request.
    /// @param config where to read from
    /// @param method HTTP method
    /// @param uri request target, query string included
    /// @return the answer
    HttpResult routeHttp(const HttpConfig &config, std::string_view method, std::string_view uri);
} // namespace mark4
