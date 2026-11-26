#pragma once

#include "file_creation_time.h"

#include <cstddef>

namespace craybridge::file_time {

// Must be called on the foobar2000 main thread. The caller supplies playlist
// coordinates only; no arbitrary path is accepted from the WebView frontend.
[[nodiscard]] QueryResult
query_playlist_item_creation_time(std::size_t playlist_index,
                                  std::size_t item_index);

} // namespace craybridge::file_time
