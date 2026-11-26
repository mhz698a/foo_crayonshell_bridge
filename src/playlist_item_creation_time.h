#pragma once

#include <SDK/foobar2000.h>

#include <Windows.h>

#include <cstddef>
#include <string>

namespace craybridge::playlist {

struct ItemPathResult {
    bool success = false;
    metadb_handle_ptr handle; // Retains the exact playlist/subsong identity.
    std::wstring physical_path;
    DWORD win32_error = ERROR_SUCCESS;
};

// Must be called on the foobar2000 main thread. Resolves only an existing
// playlist item; it performs no filesystem I/O and accepts no caller-supplied
// path. The retained handle preserves the selected playlist/subsong identity.
// Workers may use only the copied physical_path; SDK operations must return to
// the SDK-required thread.
[[nodiscard]] ItemPathResult
resolve_item_path(std::size_t playlist_index, std::size_t item_index);

} // namespace craybridge::playlist
