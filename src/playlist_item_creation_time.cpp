#include <SDK/foobar2000.h>
#include <SDK/filesystem.h>

#include "playlist_item_creation_time.h"

#include <Windows.h>

#include <limits>
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

    if (playlist_index >
            static_cast<std::size_t>((std::numeric_limits<t_size>::max)()) ||
        item_index >
            static_cast<std::size_t>((std::numeric_limits<t_size>::max)())) {
        result.win32_error = ERROR_INVALID_PARAMETER;
        return result;
    }

    try {
        auto manager = playlist_manager::get();
        const auto playlist_count = manager->get_playlist_count();

        if (static_cast<t_size>(playlist_index) >= playlist_count) {
            result.win32_error = ERROR_NOT_FOUND;
            return result;
        }

        const auto playlist = static_cast<t_size>(playlist_index);
        const auto item_count = manager->playlist_get_item_count(playlist);

        if (static_cast<t_size>(item_index) >= item_count) {
            result.win32_error = ERROR_INVALID_INDEX;
            return result;
        }

        metadb_handle_ptr handle;
        if (!manager->playlist_get_item_handle(
                handle, playlist, static_cast<t_size>(item_index))) {
            result.win32_error = ERROR_INVALID_INDEX;
            return result;
        }

        // metadb_handle::get_path() is foobar2000's canonical location,
        // not guaranteed to be a native Windows path. Normalize it through the
        // SDK first (e.g. file:// -> C:\\...); streams and virtual locations
        // without a native path must never be passed to CreateFileW.
        pfc::string8 native_path;
        if (!foobar2000_io::filesystem::g_get_native_path(handle->get_path(), native_path)) {
            result.win32_error = ERROR_NOT_SUPPORTED;
            return result;
        }
        if (native_path.length() == 0) {
            result.win32_error = ERROR_INVALID_NAME;
            return result;
        }

        if (!utf8_to_wide(native_path.c_str(),
                          result.physical_path,
                          result.win32_error)) {
            return result;
        }

        // Only the normalized native path leaves the main-thread SDK boundary.
        // The retained handle preserves the exact playlist/subsong identity.
        result.handle = handle;
        result.success = true;
        result.win32_error = ERROR_SUCCESS;
        return result;
    } catch (...) {
        result.win32_error = ERROR_GEN_FAILURE;
        return result;
    }
}

} // namespace craybridge::playlist
