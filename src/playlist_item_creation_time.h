#pragma once

#include <Windows.h>

#include <cstddef>
#include <string>

namespace craybridge::playlist {

struct ItemPathResult {
    bool success = false;
    std::wstring physical_path;
    DWORD win32_error = ERROR_SUCCESS;
};

// Must be called on the foobar2000 main thread. Resolves only an existing
// playlist item; it performs no filesystem I/O and accepts no caller-supplied
// path. The returned path can be passed to a filesystem worker.
[[nodiscard]] ItemPathResult
resolve_item_path(std::size_t playlist_index, std::size_t item_index);

} // namespace craybridge::playlist
