#pragma once

#include "http_server.h"

#include <atomic>
#include <cstdint>

namespace craybridge::api {

class ApiController final {
public:
    ApiController() noexcept;
    ApiController(const ApiController&) = delete;
    ApiController& operator=(const ApiController&) = delete;

    [[nodiscard]] http::HttpResponse handle(
        const http::Request& request) noexcept;
    void begin_shutdown() noexcept;

private:
    [[nodiscard]] http::HttpResponse health() const;
    [[nodiscard]] http::HttpResponse read_playlist_timestamps(
        std::uint64_t playlist_index,
        std::uint64_t item_index) const;

    std::uint64_t started_at_ms_ = 0;
    std::atomic<bool> shutting_down_{false};
};

} // namespace craybridge::api
