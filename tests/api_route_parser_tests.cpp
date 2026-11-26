#include "../src/api_route_parser.h"

#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

void test_playlist_timestamp_routes() {
    craybridge::api::PlaylistTimestampRoute route{};
    expect(craybridge::api::parse_playlist_timestamp_route(
               "/api/v1/playlists/0/items/0/timestamps", route),
           "canonical playlist timestamp route must parse");
    expect(route.playlist_index == 0 && route.item_index == 0,
           "playlist route indices must be retained");

    const std::string_view invalid[] = {
        "/api/v1/playlists/0/items/0/timestamps?x=1",
        "/api/v1/playlists/-1/items/0/timestamps",
        "/api/v1/playlists/0/items/0/timestamps/extra"
    };
    for (const auto value : invalid) {
        expect(!craybridge::api::parse_playlist_timestamp_route(value, route),
               "noncanonical playlist timestamp route must fail");
    }
}

void test_job_routes() {
    std::string_view id;
    expect(craybridge::api::parse_timestamp_job_route(
               "/api/v1/jobs/0123456789abcdef0123456789abcdef", id),
           "32-character lowercase hex job route must parse");
    expect(id == "0123456789abcdef0123456789abcdef",
           "job identifier must be retained");

    const std::string_view invalid[] = {
        "/api/v1/jobs/",
        "/api/v1/jobs/0123456789abcdef",
        "/api/v1/jobs/0123456789ABCDEF0123456789ABCDEF",
        "/api/v1/jobs/0123456789abcdef0123456789abcdef/extra",
        "/api/v1/jobs/0123456789abcdef0123456789abcdeg",
        "/api/v1/jobs/0123456789abcdef0123456789abcdef?x=1"
    };
    for (const auto value : invalid) {
        expect(!craybridge::api::parse_timestamp_job_route(value, id),
               "invalid job identifier route must fail");
    }
}

} // namespace

int run_api_route_parser_tests() {
    test_playlist_timestamp_routes();
    test_job_routes();

    if (g_failures != 0) {
        std::cerr << g_failures << " API route test(s) failed.\n";
        return 1;
    }

    std::cout << "All API route tests passed.\n";
    return 0;
}
