#include "file_creation_time.h"

#include <limits>

namespace craybridge::file_time {
namespace {

std::optional<std::uint64_t>
signed_filetime_to_unix_milliseconds(const LARGE_INTEGER& value) noexcept {
    if (value.QuadPart < 0) {
        return std::nullopt;
    }
    return filetime_ticks_to_unix_milliseconds(
        static_cast<std::uint64_t>(value.QuadPart));
}

bool to_filetime(std::uint64_t unixMilliseconds, FILETIME& output) noexcept {
    const auto ticks = unix_milliseconds_to_filetime_ticks(unixMilliseconds);
    if (!ticks.has_value()) {
        return false;
    }

    ULARGE_INTEGER value{};
    value.QuadPart = *ticks;
    output.dwLowDateTime = value.LowPart;
    output.dwHighDateTime = value.HighPart;
    return true;
}

TimestampQueryResult read_timestamps(HANDLE file) noexcept {
    TimestampQueryResult result{};

    FILE_BASIC_INFO basicInfo{};
    if (!::GetFileInformationByHandleEx(
            file, FileBasicInfo, &basicInfo, sizeof(basicInfo))) {
        result.win32_error = ::GetLastError();
        return result;
    }

    if ((basicInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.win32_error = ERROR_DIRECTORY;
        return result;
    }

    result.timestamps.creation_time =
        signed_filetime_to_unix_milliseconds(basicInfo.CreationTime);
    result.timestamps.last_access_time =
        signed_filetime_to_unix_milliseconds(basicInfo.LastAccessTime);
    result.timestamps.last_write_time =
        signed_filetime_to_unix_milliseconds(basicInfo.LastWriteTime);
    result.timestamps.change_time =
        signed_filetime_to_unix_milliseconds(basicInfo.ChangeTime);

    result.success = true;
    result.win32_error = ERROR_SUCCESS;
    return result;
}

HANDLE open_for_attributes(const std::wstring& path, DWORD access) noexcept {
    return ::CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
}

} // namespace

std::optional<std::uint64_t>
filetime_ticks_to_unix_milliseconds(std::uint64_t ticks) noexcept {
    if (ticks < kWindowsToUnixEpoch100ns) {
        return std::nullopt;
    }
    return (ticks - kWindowsToUnixEpoch100ns) / 10000ULL;
}

std::optional<std::uint64_t>
unix_milliseconds_to_filetime_ticks(std::uint64_t milliseconds) noexcept {
    constexpr auto maxValue = std::numeric_limits<std::uint64_t>::max();
    if (milliseconds > (maxValue - kWindowsToUnixEpoch100ns) / 10000ULL) {
        return std::nullopt;
    }
    return kWindowsToUnixEpoch100ns + milliseconds * 10000ULL;
}

TimestampQueryResult query_file_timestamps(const std::wstring& path) noexcept {
    TimestampQueryResult result{};
    if (path.empty()) {
        result.win32_error = ERROR_INVALID_PARAMETER;
        return result;
    }

    HANDLE file = open_for_attributes(path, FILE_READ_ATTRIBUTES);
    if (file == INVALID_HANDLE_VALUE) {
        result.win32_error = ::GetLastError();
        return result;
    }

    result = read_timestamps(file);
    ::CloseHandle(file);
    return result;
}

TimestampWriteResult write_file_timestamps(
    const std::wstring& path,
    const TimestampUpdate& update) noexcept {
    TimestampWriteResult result{};

    if (path.empty()) {
        result.win32_error = ERROR_INVALID_PARAMETER;
        return result;
    }

    if (update.change_time.has_value()) {
        result.win32_error = ERROR_NOT_SUPPORTED;
        return result;
    }

    if (!update.creation_time.has_value() &&
        !update.last_access_time.has_value() &&
        !update.last_write_time.has_value()) {
        result.win32_error = ERROR_INVALID_PARAMETER;
        return result;
    }

    FILETIME creationTime{};
    FILETIME lastAccessTime{};
    FILETIME lastWriteTime{};

    if ((update.creation_time.has_value() &&
         !to_filetime(*update.creation_time, creationTime)) ||
        (update.last_access_time.has_value() &&
         !to_filetime(*update.last_access_time, lastAccessTime)) ||
        (update.last_write_time.has_value() &&
         !to_filetime(*update.last_write_time, lastWriteTime))) {
        result.win32_error = ERROR_ARITHMETIC_OVERFLOW;
        return result;
    }

    HANDLE file = open_for_attributes(path, FILE_WRITE_ATTRIBUTES);
    if (file == INVALID_HANDLE_VALUE) {
        result.win32_error = ::GetLastError();
        return result;
    }

    FILE_BASIC_INFO basicInfo{};
    if (!::GetFileInformationByHandleEx(
            file, FileBasicInfo, &basicInfo, sizeof(basicInfo))) {
        result.win32_error = ::GetLastError();
        ::CloseHandle(file);
        return result;
    }

    if ((basicInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.win32_error = ERROR_DIRECTORY;
        ::CloseHandle(file);
        return result;
    }

    if (!::SetFileTime(
            file,
            update.creation_time.has_value() ? &creationTime : nullptr,
            update.last_access_time.has_value() ? &lastAccessTime : nullptr,
            update.last_write_time.has_value() ? &lastWriteTime : nullptr)) {
        result.win32_error = ::GetLastError();
        ::CloseHandle(file);
        return result;
    }

    result.success = true;
    result.win32_error = ERROR_SUCCESS;

    const auto actual = read_timestamps(file);
    ::CloseHandle(file);

    result.readback_success = actual.success;
    result.readback_error = actual.win32_error;
    if (actual.success) {
        result.timestamps = actual.timestamps;
    }
    return result;
}

QueryResult query_creation_time_unix_milliseconds(
    const std::wstring& path) noexcept {
    QueryResult result{};
    const auto query = query_file_timestamps(path);
    if (!query.success) {
        result.win32_error = query.win32_error;
        return result;
    }

    if (!query.timestamps.creation_time.has_value()) {
        result.win32_error = ERROR_INVALID_DATA;
        return result;
    }

    result.success = true;
    result.unix_milliseconds = *query.timestamps.creation_time;
    result.win32_error = ERROR_SUCCESS;
    return result;
}

} // namespace craybridge::file_time
