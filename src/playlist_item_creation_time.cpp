#include <SDK/foobar2000.h>

#include "playlist_item_creation_time.h"

#include <Windows.h>

#include <string>

namespace craybridge::file_time {
namespace {

bool utf8_to_wide(const char* utf8, std::wstring& wide, DWORD& error) {
    if (utf8 == nullptr || *utf8 == '\0') {
        error = ERROR_INVALID_NAME;
        return false;
    }

    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (required == 0) {
        error = ::GetLastError();
        return false;
    }

    wide.assign(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
            wide.data(), required) == 0) {
        error = ::GetLastError();
        wide.clear();
        return false;
    }

    // The output buffer includes the null terminator required by Win32.
    wide.resize(static_cast<std::size_t>(required - 1));
    error = ERROR_SUCCESS;
    return true;
}

} // namespace

QueryResult query_playlist_item_creation_time(std::size_t playlist_index,
                                              std::size_t item_index) {
    QueryResult result{};

    if (!core_api::is_main_thread()) {
        result.win32_error = ERROR_INVALID_THREAD_ID;
        return result;
    }

    metadb_handle_ptr handle;
    if (!playlist_manager::get()->playlist_get_item_handle(
            handle,
            static_cast<t_size>(playlist_index),
            static_cast<t_size>(item_index))) {
        result.win32_error = ERROR_INVALID_INDEX;
        return result;
    }

    std::wstring physicalPath;
    if (!utf8_to_wide(handle->get_path(), physicalPath, result.win32_error)) {
        return result;
    }

    // get_path() resolves the physical file path while the playlist handle
    // preserves subsong identity for the selected entry (e.g. CUE tracks).
    return query_creation_time_unix_milliseconds(physicalPath);
}

} // namespace craybridge::file_time
