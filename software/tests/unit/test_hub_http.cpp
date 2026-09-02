/// @file
/// @brief The HTTP surface of the hub: the pages it serves and what it
///        refuses to serve.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

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

TEST_CASE("a telemetry session round trips through the store")
{
    mark4::HttpConfig config;
    config.telemetryDir = scratchDirectory("hub_http_sessions");

    // Nothing recorded yet: an empty list, not an error. The directory
    // itself need not exist.
    const mark4::HttpResult empty = mark4::routeHttp(config, "GET", "/api/telemetry/sessions");
    REQUIRE(empty.status == mark4::HTTP_OK);
    CHECK(nlohmann::json::parse(empty.body).empty());

    const std::string session = R"({"version":1,"name":"throw_12","series":[]})";
    const mark4::HttpResult saved =
        mark4::routeHttp(config, "PUT", "/api/telemetry/sessions/throw_12", session);
    REQUIRE(saved.status == mark4::HTTP_OK);
    const nlohmann::json answer = nlohmann::json::parse(saved.body);
    CHECK(answer["name"] == "throw_12");
    CHECK(answer["bytes"] == session.size());
    // The name becomes a file name, suffix and all, so a reader outside the
    // hub knows what a file holds.
    CHECK(std::filesystem::exists(config.telemetryDir + "/sessions/throw_12.telemetry.json"));

    const mark4::HttpResult listed = mark4::routeHttp(config, "GET", "/api/telemetry/sessions");
    const nlohmann::json entries = nlohmann::json::parse(listed.body);
    REQUIRE(entries.size() == 1U);
    CHECK(entries[0]["name"] == "throw_12");
    CHECK(entries[0]["bytes"] == session.size());
    CHECK(entries[0]["modified"] != 0);

    const mark4::HttpResult read =
        mark4::routeHttp(config, "GET", "/api/telemetry/sessions/throw_12");
    REQUIRE(read.status == mark4::HTTP_OK);
    CHECK(read.body == session);
    CHECK(read.contentType == "application/json");
    // A session is opened by the page, not downloaded by the browser.
    CHECK(read.attachmentName.empty());

    // A second PUT replaces it rather than piling up.
    const std::string shorter = R"({"version":1})";
    CHECK(mark4::routeHttp(config, "PUT", "/api/telemetry/sessions/throw_12", shorter).status ==
          mark4::HTTP_OK);
    CHECK(mark4::routeHttp(config, "GET", "/api/telemetry/sessions/throw_12").body == shorter);

    CHECK(mark4::routeHttp(config, "DELETE", "/api/telemetry/sessions/throw_12").status ==
          mark4::HTTP_OK);
    CHECK(mark4::routeHttp(config, "GET", "/api/telemetry/sessions/throw_12").status ==
          mark4::HTTP_NOT_FOUND);
    CHECK(mark4::routeHttp(config, "DELETE", "/api/telemetry/sessions/throw_12").status ==
          mark4::HTTP_NOT_FOUND);
}

TEST_CASE("a csv export is stored as it comes and served as a download")
{
    mark4::HttpConfig config;
    config.telemetryDir = scratchDirectory("hub_http_exports");

    const std::string csv = "series,unit,t_s,value\nestimator/altitude,m,0.02,1.5\n";
    // The URI carries the .csv so the link a browser follows names the file
    // it saves; the stored name is the same either way.
    REQUIRE(mark4::routeHttp(config, "PUT", "/api/telemetry/exports/throw_12.csv", csv).status ==
            mark4::HTTP_OK);
    const mark4::HttpResult read =
        mark4::routeHttp(config, "GET", "/api/telemetry/exports/throw_12.csv");
    REQUIRE(read.status == mark4::HTTP_OK);
    CHECK(read.body == csv);
    CHECK(read.contentType == "text/csv; charset=utf-8");
    CHECK(read.attachmentName == "throw_12.csv");
    // A CSV is not JSON and is stored as it comes: the hub never parses it.
    CHECK(mark4::routeHttp(config, "PUT", "/api/telemetry/exports/raw.csv", "not,json\n").status ==
          mark4::HTTP_OK);
    CHECK(mark4::routeHttp(config, "GET", "/api/telemetry/exports/raw").status == mark4::HTTP_OK);
}

TEST_CASE("a view config is a small json file of its own")
{
    mark4::HttpConfig config;
    config.telemetryDir = scratchDirectory("hub_http_configs");

    const std::string view = R"({"periodMs":50,"series":[{"name":"rate/roll/error"}]})";
    REQUIRE(mark4::routeHttp(config, "PUT", "/api/telemetry/configs/tuning", view).status ==
            mark4::HTTP_OK);
    CHECK(mark4::routeHttp(config, "GET", "/api/telemetry/configs/tuning").body == view);
    const nlohmann::json entries =
        nlohmann::json::parse(mark4::routeHttp(config, "GET", "/api/telemetry/configs").body);
    REQUIRE(entries.size() == 1U);
    CHECK(entries[0]["name"] == "tuning");
    CHECK(mark4::routeHttp(config, "DELETE", "/api/telemetry/configs/tuning").status ==
          mark4::HTTP_OK);
    CHECK(nlohmann::json::parse(mark4::routeHttp(config, "GET", "/api/telemetry/configs").body)
              .empty());
}

TEST_CASE("what the telemetry store refuses")
{
    mark4::HttpConfig config;
    config.telemetryDir = scratchDirectory("hub_http_refuse");

    SECTION("a body that is not JSON where JSON is expected")
    {
        CHECK(mark4::routeHttp(config, "PUT", "/api/telemetry/sessions/bad", "{oops").status ==
              mark4::HTTP_BAD_REQUEST);
        CHECK(!std::filesystem::exists(config.telemetryDir + "/sessions/bad.telemetry.json"));
    }
    SECTION("an empty body")
    {
        CHECK(mark4::routeHttp(config, "PUT", "/api/telemetry/sessions/bad", "").status ==
              mark4::HTTP_BAD_REQUEST);
    }
    SECTION("a body over the cap")
    {
        // One byte past it, so the cap is what refuses it and not the JSON
        // parse: the body is a valid document.
        std::string big = "[\"";
        big.append(mark4::HTTP_MAX_BODY, 'x');
        big += "\"]";
        CHECK(mark4::routeHttp(config, "PUT", "/api/telemetry/sessions/big", big).status ==
              mark4::HTTP_BAD_REQUEST);
    }
    SECTION("a name that could name a file elsewhere")
    {
        for (const char *name : {"..", "../escape", "with/slash", "with.dot", ".hidden", ""})
        {
            const std::string uri = std::string("/api/telemetry/sessions/") + name;
            const int status = mark4::routeHttp(config, "PUT", uri, "{}").status;
            CHECK((status == mark4::HTTP_BAD_REQUEST || status == mark4::HTTP_NOT_FOUND));
        }
        CHECK(!mark4::validStoredName(""));
        CHECK(!mark4::validStoredName(".."));
        CHECK(!mark4::validStoredName("a/b"));
        CHECK(!mark4::validStoredName(std::string(mark4::HTTP_MAX_NAME + 1U, 'a')));
        CHECK(mark4::validStoredName("throw_12-run"));
    }
    SECTION("a method the collection does not have")
    {
        CHECK(mark4::routeHttp(config, "POST", "/api/telemetry/sessions/x", "{}").status ==
              mark4::HTTP_METHOD_NOT_ALLOWED);
        CHECK(mark4::routeHttp(config, "PUT", "/api/telemetry/sessions", "{}").status ==
              mark4::HTTP_METHOD_NOT_ALLOWED);
    }
    SECTION("a collection that does not exist")
    {
        CHECK(mark4::routeHttp(config, "GET", "/api/telemetry/nonsense").status ==
              mark4::HTTP_NOT_FOUND);
        CHECK(mark4::routeHttp(config, "GET", "/api/telemetry").status == mark4::HTTP_NOT_FOUND);
    }
    SECTION("everything, when the hub was given no telemetry directory")
    {
        mark4::HttpConfig none;
        CHECK(mark4::routeHttp(none, "GET", "/api/telemetry/sessions").status ==
              mark4::HTTP_NOT_FOUND);
        CHECK(mark4::routeHttp(none, "PUT", "/api/telemetry/sessions/x", "{}").status ==
              mark4::HTTP_NOT_FOUND);
    }
}
