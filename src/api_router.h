#pragma once

#include "http_server.h"
#include "timestamp_job_manager.h"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace craybridge::api {

class ApiController final {
public:
    ApiController();
    ApiController(const ApiController&) = delete;
    ApiController& operator=(const ApiController&) = delete;

    // Start and stop are called from the component's owned service lifecycle
    // thread, never from foobar2000's UI-thread initquit callback.
    [[nodiscard]] bool start() noexcept;
    void begin_shutdown() noexcept;
    void stop() noexcept;

    [[nodiscard]] http::HttpResponse handle(
        const http::Request& request) noexcept;

private:
    [[nodiscard]] http::HttpResponse health() const;
    [[nodiscard]] http::HttpResponse read_playlist_timestamps(
        std::uint64_t playlist_index,
        std::uint64_t item_index) const;
    [[nodiscard]] http::HttpResponse schedule_playlist_timestamp_write(
        const http::Request& request,
        std::uint64_t playlist_index,
        std::uint64_t item_index);
    [[nodiscard]] http::HttpResponse get_timestamp_job(
        std::string_view job_id) const;
    [[nodiscard]] http::HttpResponse cancel_timestamp_job(
        std::string_view job_id);

    std::uint64_t started_at_ms_ = 0;
    std::atomic<bool> shutting_down_{false};
    TimestampJobManager jobs_;
};

} // namespace craybridge::api
