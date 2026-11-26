#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace craybridge::file_time {

inline constexpr std::uint64_t kWindowsToUnixEpoch100ns =
    116444736000000000ULL;

[[nodiscard]] std::optional<std::uint64_t>
filetime_ticks_to_unix_milliseconds(std::uint64_t ticks) noexcept;

[[nodiscard]] std::optional<std::uint64_t>
unix_milliseconds_to_filetime_ticks(std::uint64_t milliseconds) noexcept;

struct QueryResult {
    bool success = false;
    std::uint64_t unix_milliseconds = 0;
    DWORD win32_error = ERROR_SUCCESS;
};

struct FileTimestamps {
    std::optional<std::uint64_t> size_bytes;
    std::optional<std::uint64_t> creation_time;
    std::optional<std::uint64_t> last_access_time;
    std::optional<std::uint64_t> last_write_time;
    std::optional<std::uint64_t> change_time;
};

struct TimestampUpdate {
    std::optional<std::uint64_t> creation_time;
    std::optional<std::uint64_t> last_access_time;
    std::optional<std::uint64_t> last_write_time;

    // Windows ChangeTime is not writable through SetFileTime.
    // Supplying this field is rejected with ERROR_NOT_SUPPORTED.
    std::optional<std::uint64_t> change_time;
};

struct TimestampQueryResult {
    bool success = false;
    FileTimestamps timestamps;
    DWORD win32_error = ERROR_SUCCESS;
};

struct TimestampWriteResult {
    bool success = false; // SetFileTime completed successfully.
    bool readback_success = false;
    FileTimestamps timestamps; // Actual values, when readback_success is true.
    DWORD win32_error = ERROR_SUCCESS;
    DWORD readback_error = ERROR_SUCCESS;
};

[[nodiscard]] TimestampQueryResult
query_file_timestamps(const std::wstring& path) noexcept;

[[nodiscard]] TimestampWriteResult
write_file_timestamps(const std::wstring& path,
                      const TimestampUpdate& update) noexcept;

// Compatibility helper retained for the initial component API.
[[nodiscard]] QueryResult
query_creation_time_unix_milliseconds(const std::wstring& path) noexcept;

} // namespace craybridge::file_time
