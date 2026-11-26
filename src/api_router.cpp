#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <SDK/foobar2000.h>

#include "api_router.h"

#include "api_route_parser.h"
#include "file_creation_time.h"
#include "playlist_item_creation_time.h"
#include "timestamp_update_json.h"
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
    const std::optional<std::uint64_t>& value,
    bool writable) {
    std::string output;
    output.reserve(160);
    output += "\"";
    output.append(name);
    output += "\":{\"supported\":true,\"readable\":";
    output += value.has_value() ? "true" : "false";
    output += ",\"writable\":";
    output += writable ? "true" : "false";

    if (value.has_value()) {
        output += ",\"unixMilliseconds\":";
        output += std::to_string(*value);
    } else {
        output += ",\"errorCode\":\"TIMESTAMP_UNAVAILABLE\"";
    }

    output += "}";
    return output;
}

void append_optional_timestamp(std::string& output,
                               std::string_view name,
                               const std::optional<std::uint64_t>& value,
                               bool& first) {
    if (!value.has_value()) return;
    if (!first) output += ",";
    first = false;
    output += "\"";
    output.append(name);
    output += "\":";
    output += std::to_string(*value);
}

std::string requested_timestamps_json(
    const file_time::TimestampUpdate& update) {
    std::string output = "{";
    bool first = true;
    append_optional_timestamp(output, "creationTime",
                              update.creation_time, first);
    append_optional_timestamp(output, "lastAccessTime",
                              update.last_access_time, first);
    append_optional_timestamp(output, "lastWriteTime",
                              update.last_write_time, first);
    output += "}";
    return output;
}

std::string actual_timestamps_json(
    const file_time::FileTimestamps& timestamps) {
    std::string output = "{";
    bool first = true;
    append_optional_timestamp(output, "creationTime",
                              timestamps.creation_time, first);
    append_optional_timestamp(output, "lastAccessTime",
                              timestamps.last_access_time, first);
    append_optional_timestamp(output, "lastWriteTime",
                              timestamps.last_write_time, first);
    append_optional_timestamp(output, "changeTime",
                              timestamps.change_time, first);
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

http::HttpResponse resolve_error_response(const ResolveDispatchResult& resolved) {
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

http::HttpResponse filesystem_error_response(DWORD error,
                                             bool writing) {
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

    case ERROR_NOT_SUPPORTED:
        return error_response(
            422, "TIMESTAMP_FIELD_NOT_WRITABLE",
            "The requested Windows timestamp field cannot be written.");

    case ERROR_INVALID_PARAMETER:
    case ERROR_ARITHMETIC_OVERFLOW:
        return error_response(
            400, "INVALID_TIMESTAMP",
            "A timestamp value is outside the supported Windows FILETIME range.");

    default:
        return error_response(
            500,
            writing ? "FILE_METADATA_WRITE_FAILED" : "FILE_METADATA_READ_FAILED",
            writing
                ? "Windows could not update the media file attributes."
                : "Windows could not read the media file attributes.");
    }
}

bool header_name_equals(std::string_view left,
                        std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool has_json_content_type(const http::Request& request) noexcept {
    const http::Header* content_type = nullptr;
    for (const auto& header : request.headers) {
        if (!header_name_equals(header.name, "content-type")) continue;
        if (content_type != nullptr) return false;
        content_type = &header;
    }

    if (content_type == nullptr) return false;

    std::string value = content_type->value;
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    value.erase(0, begin);

    for (auto& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    return value == "application/json" ||
           value == "application/json; charset=utf-8";
}

http::HttpResponse timestamp_json_error(
    TimestampUpdateJsonError error) {
    switch (error) {
    case TimestampUpdateJsonError::UnknownField:
        return error_response(
            400, "UNKNOWN_TIMESTAMP_FIELD",
            "Only creationTime, lastAccessTime and lastWriteTime are accepted.");

    case TimestampUpdateJsonError::DuplicateField:
        return error_response(
            400, "DUPLICATE_TIMESTAMP_FIELD",
            "Each timestamp field may appear only once.");

    case TimestampUpdateJsonError::UnsupportedField:
        return error_response(
            422, "TIMESTAMP_FIELD_NOT_WRITABLE",
            "Windows ChangeTime is read-only and cannot be changed by this API.");

    case TimestampUpdateJsonError::InvalidTimestamp:
        return error_response(
            422, "INVALID_TIMESTAMP",
            "Timestamp values must be unsigned Unix milliseconds within the Windows FILETIME range.");

    case TimestampUpdateJsonError::InvalidJson:
    case TimestampUpdateJsonError::None:
    default:
        return error_response(
            400, "INVALID_JSON_BODY",
            "The request body must be a JSON object containing at least one supported timestamp.");
    }
}

} // namespace

ApiController::ApiController()
    : started_at_ms_(::GetTickCount64()) {}

bool ApiController::start() noexcept {
    if (shutting_down_.load()) return false;
    return jobs_.start();
}

void ApiController::begin_shutdown() noexcept {
    shutting_down_.store(true);
}

void ApiController::stop() noexcept {
    jobs_.stop();
}

http::HttpResponse ApiController::handle(
    const http::Request& request) noexcept {
    try {
        if (shutting_down_.load()) {
            return error_response(
                503, "SERVICE_STOPPING",
                "The local metadata service is stopping.");
        }

        if (request.target == "/api/v1/system/health") {
            if (request.method != "GET") {
                return error_response(
                    405, "METHOD_NOT_ALLOWED",
                    "The health endpoint accepts GET requests only.");
            }
            return health();
        }

        PlaylistTimestampRoute playlist_route{};
        const bool is_playlist_route =
            parse_playlist_timestamp_route(request.target, playlist_route);

        std::string_view job_id;
        const bool is_job_route =
            parse_timestamp_job_route(request.target, job_id);

        if (request.method == "GET" && is_playlist_route) {
            return read_playlist_timestamps(
                playlist_route.playlist_index,
                playlist_route.item_index);
        }

        if (request.method == "PATCH" && is_playlist_route) {
            return schedule_playlist_timestamp_write(
                request,
                playlist_route.playlist_index,
                playlist_route.item_index);
        }

        if (request.method == "GET" && is_job_route) {
            return get_timestamp_job(job_id);
        }

        if (request.method == "DELETE" && is_job_route) {
            return cancel_timestamp_job(job_id);
        }

        if (is_playlist_route || is_job_route) {
            return error_response(
                405, "METHOD_NOT_ALLOWED",
                "The HTTP method is not supported for this endpoint.");
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
        "\"readFileSize\":true,"
        "\"readCreationTime\":true,"
        "\"writeCreationTime\":true,"
        "\"readLastAccessTime\":true,"
        "\"writeLastAccessTime\":true,"
        "\"readLastWriteTime\":true,"
        "\"writeLastWriteTime\":true,"
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

    if (resolved.state != ResolveDispatchState::Completed) {
        return resolve_error_response(resolved);
    }
    if (!resolved.item.success) {
        return resolve_error_response(resolved);
    }

    // The retained handle preserves subsong identity. The worker uses only
    // the copied physical path for Windows metadata I/O.
    const auto timestamps =
        file_time::query_file_timestamps(resolved.item.physical_path);

    if (!timestamps.success) {
        return filesystem_error_response(timestamps.win32_error, false);
    }

    std::string body =
        "{\"success\":true,\"data\":{\"playlistIndex\":";
    body += std::to_string(playlist_index);
    body += ",\"itemIndex\":";
    body += std::to_string(item_index);
    body += ",\"fileSizeBytes\":";
    body += timestamps.timestamps.size_bytes.has_value()
        ? std::to_string(*timestamps.timestamps.size_bytes)
        : "null";
    body += ",\"timestamps\":{";
    body += timestamp_field("creationTime",
                            timestamps.timestamps.creation_time, true);
    body += ",";
    body += timestamp_field("lastAccessTime",
                            timestamps.timestamps.last_access_time, true);
    body += ",";
    body += timestamp_field("lastWriteTime",
                            timestamps.timestamps.last_write_time, true);
    body += ",";
    body += timestamp_field("changeTime",
                            timestamps.timestamps.change_time, false);
    body += "}}}";

    return {200, std::move(body)};
}

http::HttpResponse ApiController::schedule_playlist_timestamp_write(
    const http::Request& request,
    std::uint64_t playlist_index,
    std::uint64_t item_index) {
    if (!has_json_content_type(request)) {
        return error_response(
            415, "UNSUPPORTED_MEDIA_TYPE",
            "Timestamp updates require Content-Type application/json.");
    }

    file_time::TimestampUpdate update{};
    TimestampUpdateJsonError parse_error{};
    if (!parse_timestamp_update_json(request.body, update, parse_error)) {
        return timestamp_json_error(parse_error);
    }

    // Resolve the row now, before enqueuing. The queued job stores only this
    // internally resolved path, so later playlist reorderings cannot redirect it.
    const auto resolved = resolve_playlist_item_on_main_thread(
        playlist_index, item_index);

    if (resolved.state != ResolveDispatchState::Completed) {
        return resolve_error_response(resolved);
    }
    if (!resolved.item.success) {
        return resolve_error_response(resolved);
    }

    std::string job_id;
    const auto submitted = jobs_.submit(
        resolved.item.physical_path,
        playlist_index,
        item_index,
        update,
        job_id);

    switch (submitted) {
    case TimestampJobSubmitStatus::QueueFull:
        return error_response(
            503, "JOB_QUEUE_FULL",
            "The timestamp write queue is full. Retry after existing jobs finish.");
    case TimestampJobSubmitStatus::ServiceStopping:
        return error_response(
            503, "SERVICE_STOPPING",
            "The local metadata service is stopping.");
    case TimestampJobSubmitStatus::IdentifierFailure:
        return error_response(
            500, "JOB_IDENTIFIER_FAILURE",
            "A secure timestamp job identifier could not be generated.");
    case TimestampJobSubmitStatus::InternalError:
        return error_response(
            500, "JOB_SUBMISSION_FAILED",
            "The timestamp write job could not be queued.");
    case TimestampJobSubmitStatus::Accepted:
        break;
    }

    std::string body =
        "{\"success\":true,\"data\":{\"jobId\":\"" + job_id +
        "\",\"status\":\"queued\",\"playlistIndex\":" +
        std::to_string(playlist_index) +
        ",\"itemIndex\":" + std::to_string(item_index) +
        ",\"statusUrl\":\"/api/v1/jobs/" + job_id + "\"}}";
    return {202, std::move(body)};
}

http::HttpResponse ApiController::get_timestamp_job(
    std::string_view job_id) const {
    TimestampJobSnapshot snapshot{};
    if (!jobs_.query(job_id, snapshot)) {
        return error_response(
            404, "JOB_NOT_FOUND",
            "The requested timestamp job does not exist or has expired.");
    }

    std::string body =
        "{\"success\":true,\"data\":{\"jobId\":\"" + snapshot.id +
        "\",\"status\":\"" + snapshot.status +
        "\",\"playlistIndex\":" + std::to_string(snapshot.playlist_index) +
        ",\"itemIndex\":" + std::to_string(snapshot.item_index) +
        ",\"requested\":" + requested_timestamps_json(snapshot.requested);

    if (snapshot.has_result) {
        body += ",\"filesystem\":{\"writeSucceeded\":";
        body += snapshot.result.success ? "true" : "false";
        body += ",\"writeErrorCode\":";
        body += std::to_string(snapshot.result.win32_error);
        body += ",\"readbackSucceeded\":";
        body += snapshot.result.readback_success ? "true" : "false";
        body += ",\"readbackErrorCode\":";
        body += std::to_string(snapshot.result.readback_error);
        if (snapshot.result.readback_success) {
            body += ",\"actual\":";
            body += actual_timestamps_json(snapshot.result.timestamps);
        }
        body += "}";
    }

    body += ",\"metadbRefresh\":{\"status\":\"not_attempted\","
            "\"available\":false}}}";
    return {200, std::move(body)};
}

http::HttpResponse ApiController::cancel_timestamp_job(
    std::string_view job_id) {
    switch (jobs_.cancel(job_id)) {
    case TimestampJobCancelStatus::Cancelled:
        return {200,
            "{\"success\":true,\"data\":{\"jobId\":\"" +
            std::string(job_id) + "\",\"status\":\"cancelled\"}}"};
    case TimestampJobCancelStatus::Running:
        return error_response(
            409, "JOB_ALREADY_RUNNING",
            "A timestamp job cannot be cancelled after its filesystem operation has started.");
    case TimestampJobCancelStatus::AlreadyCompleted:
        return error_response(
            409, "JOB_ALREADY_COMPLETED",
            "A completed or cancelled timestamp job cannot be cancelled.");
    case TimestampJobCancelStatus::NotFound:
    default:
        return error_response(
            404, "JOB_NOT_FOUND",
            "The requested timestamp job does not exist or has expired.");
    }
}

} // namespace craybridge::api
