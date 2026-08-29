/// @file
/// @brief The HTTP surface of the hub: the pages it serves and what it
///        refuses to serve.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "hub/http_api.hpp"

namespace
{
    /// @brief Private directory for one test, emptied beforehand.
    /// @param name directory name
    /// @return the path
    std::string scratchDirectory(const char *name)
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::error_code failure;
        std::filesystem::remove_all(path, failure);
        std::filesystem::create_directories(path, failure);
        return path.string();
    }

    /// @brief Writes one file.
    /// @param path file to write
    /// @param content what to put in it
    void writeFile(const std::string &path, const std::string &content)
    {
        std::error_code failure;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), failure);
        std::ofstream file(path, std::ios::binary);
        file << content;
    }

    /// @brief Builds a pages directory with a few files in it.
    /// @param name directory name
    /// @return the path
    std::string pagesDirectory(const char *name)
    {
        const std::string pages = scratchDirectory(name);
        std::error_code failure;
        std::filesystem::create_directories(pages + "/assets", failure);
        writeFile(pages + "/index.html", "<!doctype html><title>hub</title>");
        writeFile(pages + "/assets/app.js", "export const answer = 42;\n");
        writeFile(pages + "/assets/style.css", "body { margin: 0 }\n");
        return pages;
    }
} // namespace

TEST_CASE("a file is typed by its extension, and javascript by the only type that runs")
{
    // Served under anything else, a browser refuses to run an ES module.
    CHECK(mark4::mimeTypeOf("app.js") == "text/javascript; charset=utf-8");
    CHECK(mark4::mimeTypeOf("app.mjs") == "text/javascript; charset=utf-8");
    CHECK(mark4::mimeTypeOf("index.html") == "text/html; charset=utf-8");
    CHECK(mark4::mimeTypeOf("style.css") == "text/css; charset=utf-8");
    CHECK(mark4::mimeTypeOf("icon.svg") == "image/svg+xml");
    CHECK(mark4::mimeTypeOf("data.json") == "application/json");
    CHECK(mark4::mimeTypeOf("run.csv") == "text/csv; charset=utf-8");
    CHECK(mark4::mimeTypeOf("favicon.ico") == "image/x-icon");
    CHECK(mark4::mimeTypeOf("shot.png") == "image/png");
    CHECK(mark4::mimeTypeOf("noextension") == "application/octet-stream");
}

TEST_CASE("the root of the endpoint is the index page")
{
    mark4::HttpConfig config;
    config.pagesDir = pagesDirectory("hub_http_pages");

    const mark4::HttpResult root = mark4::routeHttp(config, "GET", "/");
    CHECK(root.status == mark4::HTTP_OK);
    CHECK(root.contentType == "text/html; charset=utf-8");
    CHECK(root.body == "<!doctype html><title>hub</title>");
    CHECK(mark4::routeHttp(config, "GET", "").body == root.body);
    CHECK(mark4::routeHttp(config, "GET", "/index.html").body == root.body);
    CHECK(mark4::routeHttp(config, "GET", "/?anything=1").body == root.body);
}

TEST_CASE("a file below the pages directory is served, one outside it is not")
{
    mark4::HttpConfig config;
    config.pagesDir = pagesDirectory("hub_http_below");

    const mark4::HttpResult script = mark4::routeHttp(config, "GET", "/assets/app.js");
    CHECK(script.status == mark4::HTTP_OK);
    CHECK(script.contentType == "text/javascript; charset=utf-8");

    CHECK(mark4::routeHttp(config, "GET", "/assets/missing.js").status == mark4::HTTP_NOT_FOUND);
    // Nothing outside the pages directory is reachable, however it is spelled.
    CHECK(mark4::routeHttp(config, "GET", "/../../etc/passwd").status == mark4::HTTP_NOT_FOUND);
    CHECK(mark4::routeHttp(config, "GET", "/assets/../../etc/passwd").status ==
          mark4::HTTP_NOT_FOUND);
    CHECK(mark4::routeHttp(config, "GET", "//etc/passwd").status == mark4::HTTP_NOT_FOUND);
    CHECK(mark4::routeHttp(config, "GET", "/assets").status == mark4::HTTP_NOT_FOUND);
}

TEST_CASE("a hub without pages answers a 404 rather than refusing to start")
{
    mark4::HttpConfig config;
    config.pagesDir = "/nowhere/at/all";
    CHECK(mark4::routeHttp(config, "GET", "/").status == mark4::HTTP_NOT_FOUND);

    config.pagesDir.clear();
    CHECK(mark4::routeHttp(config, "GET", "/").status == mark4::HTTP_NOT_FOUND);
}

TEST_CASE("anything but a read is refused")
{
    mark4::HttpConfig config;
    config.pagesDir = pagesDirectory("hub_http_method");
    CHECK(mark4::routeHttp(config, "POST", "/").status == mark4::HTTP_METHOD_NOT_ALLOWED);
    CHECK(mark4::routeHttp(config, "DELETE", "/api/nothing").status ==
          mark4::HTTP_METHOD_NOT_ALLOWED);
    CHECK(mark4::routeHttp(config, "HEAD", "/").status == mark4::HTTP_OK);
}
