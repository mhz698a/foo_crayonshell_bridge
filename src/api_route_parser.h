#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace craybridge::api {

struct PlaylistTimestampRoute {
    std::uint64_t playlist_index = 0;
    std::uint64_t item_index = 0;
};

namespace detail {
inline bool parse_route_index(std::string_view value,
                              std::uint64_t& output) noexcept {
    if (value.empty()) return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        return false;
    }
    output = parsed;
    return true;
}

inline bool is_lower_hex_job_id(std::string_view value) noexcept {
    if (value.size() != 32) return false;
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}
} // namespace detail

// Only the canonical route is accepted. Query strings, encoded separators,
// signs, empty values, extra path segments and integer overflow are rejected.
[[nodiscard]] inline bool parse_playlist_timestamp_route(
    std::string_view target,
    PlaylistTimestampRoute& output) noexcept {
    constexpr std::string_view prefix = "/api/v1/playlists/";
    constexpr std::string_view item_separator = "/items/";
    constexpr std::string_view suffix = "/timestamps";

    if (!target.starts_with(prefix)) return false;
    target.remove_prefix(prefix.size());

    const auto marker = target.find(item_separator);
    if (marker == std::string_view::npos) return false;

    const auto playlist_text = target.substr(0, marker);
    target.remove_prefix(marker + item_separator.size());

    if (!target.ends_with(suffix)) return false;
    target.remove_suffix(suffix.size());
    if (target.find('/') != std::string_view::npos) return false;

    PlaylistTimestampRoute parsed{};
    if (!detail::parse_route_index(playlist_text, parsed.playlist_index) ||
        !detail::parse_route_index(target, parsed.item_index)) {
        return false;
    }

    output = parsed;
    return true;
}

[[nodiscard]] inline bool parse_timestamp_job_route(
    std::string_view target,
    std::string_view& job_id) noexcept {
    constexpr std::string_view prefix = "/api/v1/jobs/";
    if (!target.starts_with(prefix)) return false;

    const auto candidate = target.substr(prefix.size());
    if (!detail::is_lower_hex_job_id(candidate)) return false;

    job_id = candidate;
    return true;
}

} // namespace craybridge::api
