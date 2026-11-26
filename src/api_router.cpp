#include "api_router.h"

#include <SDK/foobar2000.h>

#include "api_route_parser.h"
#include "file_creation_time.h"
#include "playlist_item_creation_time.h"
#include "version.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace craybridge::api {
namespace {

constexpr auto kMainThreadDispatchTimeout = std::chrono::milliseconds(1500);

std::string make_error_body(std::string_view code,
                            std::string_view message) {
    return std::string("{\"success\":false,\"error\":{\"code\":\"") +
        std::string(code) + "\",\"message\":\"" +
        std::string(message) + "\"}}";
}

http::HttpResponse error_response(unsigned int status,
                                  std::string_view code,
                                  std::string_view message) {
    return {status, make_error_body(code, message)};
}

std::string timestamp_field(
    std::string_view name,
    const std::optional<std::uint64_t>& value) {
    std::string output;
    output.reserve(160);
    output += "\"";
    output.append(name);
    output += "\":{\"supported\":true,\"readable\":";
    output += value.has_value() ? "true" : "false";
    output += ",\"writable\":false";

    if (value.has_value()) {
        output += ",\"unixMilliseconds\":";
        output += std::to_string(*value);
    } else {
        output += ",\"errorCode\":\"TIMESTAMP_UNAVAILABLE\"";
    }

    output += "}";
    return output;
}

struct MainThreadResolveState {
    std::mutex mutex;
    std::condition_variable condition;
    bool cancelled = false;
    bool completed = false;
    playlist::ItemPathResult result;
};

enum class ResolveDispatchState {
    Completed,
    TimedOut,
    ShuttingDown
};

struct ResolveDispatchResult {
    ResolveDispatchState state = ResolveDispatchState::TimedOut;
    playlist::ItemPathResult item;
};

ResolveDispatchResult resolve_playlist_item_on_main_thread(
    std::uint64_t playlist_index,
    std::uint64_t item_index) {
    if (core_api::is_shutting_down() ||
        !core_api::are_services_available()) {
        return {ResolveDispatchState::ShuttingDown, {}};
    }

    auto state = std::make_shared<MainThreadResolveState>();

    try {
        fb2k::inMainThread([state, playlist_index, item_index] {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->cancelled) return;
            }

            playlist::ItemPathResult result{};
            try {
                if (playlist_index > static_cast<std::uint64_t>(
                        (std::numeric_limits<t_size>::max)()) ||
                    item_index > static_cast<std::uint64_t>(
                        (std::numeric_limits<t_size>::max)())) {
                    result.win32_error = ERROR_INVALID_PARAMETER;
                } else {
                    result = playlist::resolve_item_path(
                        static_cast<std::size_t>(playlist_index),
                        static_cast<std::size_t>(item_index));
                }
            } catch (...) {
                result.win32_error = ERROR_GEN_FAILURE;
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->cancelled) return;
                state->result = std::move(result);
                state->completed = true;
            }
            state->condition.notify_one();
        });
    } catch (...) {
        return {ResolveDispatchState::ShuttingDown, {}};
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(
            lock,
            kMainThreadDispatchTimeout,
            [&state] { return state->completed || state->cancelled; })) {
        // If the SDK callback is still queued, it becomes a no-op on dequeue.
        state->cancelled = true;
        return {ResolveDispatchState::TimedOut, {}};
    }

    if (!state->completed) {
        return {ResolveDispatchState::ShuttingDown, {}};
    }

    return {ResolveDispatchState::Completed, std::move(state->result)};
}

http::HttpResponse filesystem_error_response(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return error_response(
            404, "FILE_NOT_FOUND",
            "The media file referenced by the playlist item was not found.");

    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return error_response(
            403, "FILE_ACCESS_DENIED",
            "Windows denied access to the media file attributes.");

    case ERROR_DIRECTORY:
        return error_response(
            422, "ITEM_NOT_SUPPORTED",
            "The selected playlist item does not resolve to a media file.");

    default:
        return error_response(
            500, "FILE_METADATA_READ_FAILED",
            "Windows could not read the media file attributes.");
    }
}

} // namespace

ApiController::ApiController() noexcept
    : started_at_ms_(::GetTickCount64()) {}

void ApiController::begin_shutdown() noexcept {
    shutting_down_.store(true);
}

http::HttpResponse ApiController::handle(
    const http::Request& request) noexcept {
    try {
        if (shutting_down_.load()) {
            return error_response(
                503, "SERVICE_STOPPING",
                "The local metadata service is stopping.");
        }

        if (request.method != "GET") {
            return error_response(
                405, "METHOD_NOT_ALLOWED",
                "This endpoint accepts GET requests only.");
        }

        if (request.target == "/api/v1/system/health") {
            return health();
        }

        PlaylistTimestampRoute route{};
        if (parse_playlist_timestamp_route(request.target, route)) {
            return read_playlist_timestamps(
                route.playlist_index,
                route.item_index);
        }

        return error_response(
            404, "ROUTE_NOT_FOUND",
            "The requested API route does not exist.");
    } catch (...) {
        return error_response(
            500, "INTERNAL_ERROR",
            "The local metadata service could not process the request.");
    }
}

http::HttpResponse ApiController::health() const {
    const auto now = ::GetTickCount64();
    const auto uptime = now >= started_at_ms_ ? now - started_at_ms_ : 0;

    std::string body =
        "{\"success\":true,\"data\":{"
        "\"protocolVersion\":1,"
        "\"componentVersion\":\"" CRAYONSHELL_BRIDGE_VERSION "\","
        "\"uptimeMs\":";
    body += std::to_string(uptime);
    body +=
        ",\"capabilities\":{"
        "\"readCreationTime\":true,"
        "\"writeCreationTime\":false,"
        "\"readLastAccessTime\":true,"
        "\"writeLastAccessTime\":false,"
        "\"readLastWriteTime\":true,"
        "\"writeLastWriteTime\":false,"
        "\"readChangeTime\":true,"
        "\"writeChangeTime\":false,"
        "\"metadbRefresh\":false"
        "}}}";

    return {200, std::move(body)};
}

http::HttpResponse ApiController::read_playlist_timestamps(
    std::uint64_t playlist_index,
    std::uint64_t item_index) const {
    const auto resolved = resolve_playlist_item_on_main_thread(
        playlist_index, item_index);

    if (resolved.state == ResolveDispatchState::TimedOut) {
        return error_response(
            503, "SDK_DISPATCH_TIMEOUT",
            "foobar2000 did not process the playlist lookup before the deadline.");
    }

    if (resolved.state == ResolveDispatchState::ShuttingDown) {
        return error_response(
            503, "SERVICE_STOPPING",
            "foobar2000 is shutting down or its SDK services are unavailable.");
    }

    if (!resolved.item.success) {
        switch (resolved.item.win32_error) {
        case ERROR_NOT_FOUND:
            return error_response(
                404, "INVALID_PLAYLIST_INDEX",
                "The requested playlist does not exist.");

        case ERROR_INVALID_INDEX:
            return error_response(
                404, "INVALID_ITEM_INDEX",
                "The requested item does not exist in the playlist.");

        case ERROR_INVALID_PARAMETER:
            return error_response(
                400, "INVALID_PLAYLIST_COORDINATES",
                "The playlist or item index is outside the supported range.");

        case ERROR_INVALID_THREAD_ID:
            return error_response(
                500, "SDK_THREAD_VIOLATION",
                "The playlist lookup was not executed on the foobar2000 main thread.");

        default:
            return error_response(
                500, "PLAYLIST_LOOKUP_FAILED",
                "foobar2000 could not resolve the requested playlist item.");
        }
    }

    // The retained handle preserves subsong identity. The worker uses only
    // the copied physical path for Windows metadata I/O.
    const auto timestamps =
        file_time::query_file_timestamps(resolved.item.physical_path);

    if (!timestamps.success) {
        return filesystem_error_response(timestamps.win32_error);
    }

    std::string body =
        "{\"success\":true,\"data\":{\"playlistIndex\":";
    body += std::to_string(playlist_index);
    body += ",\"itemIndex\":";
    body += std::to_string(item_index);
    body += ",\"timestamps\":{";
    body += timestamp_field("creationTime",
                            timestamps.timestamps.creation_time);
    body += ",";
    body += timestamp_field("lastAccessTime",
                            timestamps.timestamps.last_access_time);
    body += ",";
    body += timestamp_field("lastWriteTime",
                            timestamps.timestamps.last_write_time);
    body += ",";
    body += timestamp_field("changeTime",
                            timestamps.timestamps.change_time);
    body += "}}}";

    return {200, std::move(body)};
}

} // namespace craybridge::api
