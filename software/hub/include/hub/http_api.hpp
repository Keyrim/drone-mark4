#pragma once

/// @file
/// @brief HTTP side of the hub endpoint: the static pages the browser loads,
///        served on the same TCP port as the websocket so a page never has to
///        know a port of its own.
///
///        Pure routing: a method and a URI go in, a response comes out. No
///        socket, no state, so a unit test drives the whole surface.
///
///        THE INVARIANT: the HTTP side is filesystem-only; everything live
///        is websocket. These handlers run on the connection threads of the
///        websocket library, so they must never touch the discovery registry
///        or the counters. That is what keeps the hub free of locks.

#include <cstddef>
#include <string>
#include <string_view>

namespace mark4
{
    /// HTTP status codes the hub answers with.
    inline constexpr int HTTP_OK = 200;
    inline constexpr int HTTP_BAD_REQUEST = 400;
    inline constexpr int HTTP_NOT_FOUND = 404;
    inline constexpr int HTTP_METHOD_NOT_ALLOWED = 405;

    /// Largest body one telemetry request may carry [bytes]. A recording is
    /// a page-built JSON document and a session of a few minutes at 50 Hz is
    /// a few MB; this is the point past which the client is confused rather
    /// than verbose.
    inline constexpr std::size_t HTTP_MAX_BODY = 64U * 1024U * 1024U;

    /// Longest stored name, and the characters one may hold: the same rule
    /// as a tuning profile's, because the reason is the same - the name
    /// becomes a file name.
    inline constexpr std::size_t HTTP_MAX_NAME = 64U;

    /// Where the HTTP side reads from. Filled once before the endpoint starts
    /// serving and never written again: the connection threads only read it.
    struct HttpConfig
    {
        std::string pagesDir;     ///< directory the static pages are read
                                  ///< from, empty or missing = every page is
                                  ///< a 404
        std::string telemetryDir; ///< directory the CSV exports and the named
                                  ///< view configs live in, empty = every
                                  ///< /api/telemetry request is a 404
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
    /// @param body request body, empty for the methods that carry none
    /// @return the answer
    HttpResult routeHttp(const HttpConfig &config,
                         std::string_view method,
                         std::string_view uri,
                         std::string_view body = {});

    /// @brief Says whether a name may become a stored file name: not empty,
    ///        at most HTTP_MAX_NAME characters, letters, digits, `_` and `-`
    ///        only. No separator, no leading dot, so a request can never
    ///        name a file outside its own directory.
    /// @param name candidate name
    /// @return true when it may be stored
    bool validStoredName(std::string_view name);
} // namespace mark4
