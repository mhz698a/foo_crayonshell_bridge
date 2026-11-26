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

struct QueryResult {
    bool success = false;
    std::uint64_t unix_milliseconds = 0;
    DWORD win32_error = ERROR_SUCCESS;
};

[[nodiscard]] QueryResult
query_creation_time_unix_milliseconds(const std::wstring& path) noexcept;

} // namespace craybridge::file_time
