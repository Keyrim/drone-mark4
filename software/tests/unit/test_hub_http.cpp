/// @file
/// @brief The HTTP surface of the hub: the pages it serves, what it refuses
///        to serve, and the shape of the answers a page reads.

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>

#include "hub/http_api.hpp"
#include "hub/stream_recorder.hpp"

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

    /// @brief Records one CSV pair so the API has something to answer about.
    /// @param logDir directory to record into
    void recordPair(const std::string &logDir)
    {
        mark4::StreamRecorder recorder(logDir);
        REQUIRE(recorder.startCsvSession());
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            mark4::TelemetryPacket telemetry{};
            telemetry.timestampUs = 1000U * (index + 1U);
            telemetry.attitudeQuat = {1.0f, 0.0f, 0.0f, 0.0f};
            recorder.onTelemetry(telemetry);
            mark4::SimRawPacket simRaw{};
            simRaw.timestampUs = 1000U * (index + 1U);
            simRaw.attitudeQuat = {1.0f, 0.0f, 0.0f, 0.0f};
            recorder.onSimRaw(simRaw);
        }
        recorder.stopCsvSession();
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
    CHECK(mark4::mimeTypeOf("log.m4bb") == "application/octet-stream");
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
    CHECK(mark4::routeHttp(config, "DELETE", "/api/recordings").status ==
          mark4::HTTP_METHOD_NOT_ALLOWED);
    CHECK(mark4::routeHttp(config, "HEAD", "/").status == mark4::HTTP_OK);
}

TEST_CASE("the api lists what the log directory holds")
{
    mark4::HttpConfig config;
    config.logDir = scratchDirectory("hub_http_api_list");
    recordPair(config.logDir);

    const mark4::HttpResult answer = mark4::routeHttp(config, "GET", "/api/recordings");
    REQUIRE(answer.status == mark4::HTTP_OK);
    CHECK(answer.contentType == "application/json");
    const nlohmann::json body = nlohmann::json::parse(answer.body);
    CHECK(body["logDir"] == config.logDir);
    REQUIRE(body["recordings"].size() == 1U);
    CHECK(body["recordings"][0]["kind"] == "streams");
}

TEST_CASE("the api decodes one recording by name")
{
    mark4::HttpConfig config;
    config.logDir = scratchDirectory("hub_http_api_decode");
    recordPair(config.logDir);
    const std::string name = nlohmann::json::parse(
        mark4::routeHttp(config, "GET", "/api/recordings").body)["recordings"][0]["name"];

    const mark4::HttpResult answer =
        mark4::routeHttp(config, "GET", "/api/recording?name=" + name + "&maxPoints=2");
    REQUIRE(answer.status == mark4::HTTP_OK);
    const nlohmann::json body = nlohmann::json::parse(answer.body);
    CHECK(body["name"] == name);
    CHECK(body["kind"] == "streams");
    CHECK(body["telemetry"]["total"] == 4U);
    CHECK(body["telemetry"]["stride"] == 2U);
    CHECK(body["telemetry"]["rows"].is_array());
}

TEST_CASE("an api request that names nothing real is refused with a reason")
{
    mark4::HttpConfig config;
    config.logDir = scratchDirectory("hub_http_api_errors");

    const mark4::HttpResult noName = mark4::routeHttp(config, "GET", "/api/recording");
    CHECK(noName.status == mark4::HTTP_BAD_REQUEST);
    CHECK(nlohmann::json::parse(noName.body)["error"] == "name is required");

    const mark4::HttpResult unknown =
        mark4::routeHttp(config, "GET", "/api/recording?name=nothing");
    CHECK(unknown.status == mark4::HTTP_NOT_FOUND);
    CHECK(nlohmann::json::parse(unknown.body)["error"] == "no recording named nothing");

    // A name is looked up in the listing and never turned into a path, so a
    // traversal is simply a name nothing answers to.
    const mark4::HttpResult traversal =
        mark4::routeHttp(config, "GET", "/api/recording?name=..%2F..%2Fetc%2Fpasswd");
    CHECK(traversal.status == mark4::HTTP_NOT_FOUND);
    CHECK(nlohmann::json::parse(traversal.body)["error"] == "no recording named ../../etc/passwd");

    const mark4::HttpResult endpoint = mark4::routeHttp(config, "GET", "/api/nonsense");
    CHECK(endpoint.status == mark4::HTTP_NOT_FOUND);
    CHECK(nlohmann::json::parse(endpoint.body)["error"] == "no such endpoint");
}

TEST_CASE("the api scores one recorded pair and downloads its files")
{
    mark4::HttpConfig config;
    config.logDir = scratchDirectory("hub_http_api_compare");
    recordPair(config.logDir);
    const std::string name = nlohmann::json::parse(
        mark4::routeHttp(config, "GET", "/api/recordings").body)["recordings"][0]["name"];

    const mark4::HttpResult scored = mark4::routeHttp(config, "GET", "/api/compare?name=" + name);
    REQUIRE(scored.status == mark4::HTTP_OK);
    const nlohmann::json body = nlohmann::json::parse(scored.body);
    CHECK(body["maxGapUs"] == 30000U);
    CHECK(body["alignedSamples"] == 4U);
    CHECK(body["metrics"].size() == 3U);
    CHECK(body["series"]["columns"][0] == "timestamp_us");

    const mark4::HttpResult download =
        mark4::routeHttp(config, "GET", "/api/file?name=" + name + "&part=simraw");
    CHECK(download.status == mark4::HTTP_OK);
    CHECK(download.contentType == "text/csv; charset=utf-8");
    CHECK(download.attachmentName == name + "_simraw.csv");
    CHECK(download.body.rfind("timestamp_us,quat_w", 0U) == 0U);

    // A pair defaults to its estimated half: that is the one a human means.
    CHECK(mark4::routeHttp(config, "GET", "/api/file?name=" + name).attachmentName ==
          name + "_telemetry.csv");
    CHECK(mark4::routeHttp(config, "GET", "/api/file?name=" + name + "&part=raw").status ==
          mark4::HTTP_BAD_REQUEST);
}

TEST_CASE("a summary is a blackbox thing and a comparison a streams thing")
{
    mark4::HttpConfig config;
    config.logDir = scratchDirectory("hub_http_api_kinds");
    recordPair(config.logDir);
    const std::string name = nlohmann::json::parse(
        mark4::routeHttp(config, "GET", "/api/recordings").body)["recordings"][0]["name"];

    const mark4::HttpResult summary = mark4::routeHttp(config, "GET", "/api/summary?name=" + name);
    CHECK(summary.status == mark4::HTTP_BAD_REQUEST);
    CHECK(nlohmann::json::parse(summary.body)["error"] == name + " is not a blackbox recording");

    // And the other way round: a blackbox file has no exact state beside it.
    writeFile(config.logDir + "/blackbox/board_20260101_000000.m4bb", std::string());
    const mark4::HttpResult compare =
        mark4::routeHttp(config, "GET", "/api/compare?name=board_20260101_000000.m4bb");
    CHECK(compare.status == mark4::HTTP_BAD_REQUEST);
}

TEST_CASE("a window that asks for nothing sensible is refused")
{
    mark4::HttpConfig config;
    config.logDir = scratchDirectory("hub_http_api_window");
    recordPair(config.logDir);
    const std::string name = nlohmann::json::parse(
        mark4::routeHttp(config, "GET", "/api/recordings").body)["recordings"][0]["name"];

    const mark4::HttpResult backwards =
        mark4::routeHttp(config, "GET", "/api/recording?name=" + name + "&from=9&to=1");
    CHECK(backwards.status == mark4::HTTP_BAD_REQUEST);
    CHECK(nlohmann::json::parse(backwards.body)["error"] == "from must not be after to");

    const mark4::HttpResult none =
        mark4::routeHttp(config, "GET", "/api/recording?name=" + name + "&maxPoints=0");
    CHECK(none.status == mark4::HTTP_BAD_REQUEST);
}
