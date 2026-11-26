#include "file_creation_time.h"

namespace craybridge::file_time {

std::optional<std::uint64_t>
filetime_ticks_to_unix_milliseconds(std::uint64_t ticks) noexcept {
    if (ticks < kWindowsToUnixEpoch100ns) {
        return std::nullopt;
    }
    return (ticks - kWindowsToUnixEpoch100ns) / 10000ULL;
}

QueryResult query_creation_time_unix_milliseconds(const std::wstring& path) noexcept {
    QueryResult result{};

    if (path.empty()) {
        result.win32_error = ERROR_INVALID_PARAMETER;
        return result;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!::GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &attributes)) {
        result.win32_error = ::GetLastError();
        return result;
    }

    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.win32_error = ERROR_DIRECTORY;
        return result;
    }

    ULARGE_INTEGER creationTicks{};
    creationTicks.LowPart = attributes.ftCreationTime.dwLowDateTime;
    creationTicks.HighPart = attributes.ftCreationTime.dwHighDateTime;

    const auto timestamp =
        filetime_ticks_to_unix_milliseconds(creationTicks.QuadPart);

    if (!timestamp.has_value()) {
        result.win32_error = ERROR_INVALID_DATA;
        return result;
    }

    result.success = true;
    result.unix_milliseconds = *timestamp;
    result.win32_error = ERROR_SUCCESS;
    return result;
}

} // namespace craybridge::file_time
