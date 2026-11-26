#include <SDK/foobar2000.h>

#include "playlist_item_creation_time.h"

#include <Windows.h>

#include <string>

namespace craybridge::playlist {
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

    wide.resize(static_cast<std::size_t>(required - 1));
    error = ERROR_SUCCESS;
    return true;
}

} // namespace

ItemPathResult resolve_item_path(std::size_t playlist_index,
                                 std::size_t item_index) {
    ItemPathResult result{};

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

    if (!utf8_to_wide(handle->get_path(),
                      result.physical_path,
                      result.win32_error)) {
        return result;
    }

    // This is the physical media path. The selected metadb_handle remains the
    // authority for the playlist/subsong identity; only the copied path leaves
    // the main-thread SDK boundary. No disk operation runs here.
    result.success = true;
    result.win32_error = ERROR_SUCCESS;
    return result;
}

} // namespace craybridge::playlist
