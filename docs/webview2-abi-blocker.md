# External API registration: ABI prerequisite

## Finding

The current foo_ui_webview2 source documents external API registration through GetPluginRegistry(), PluginRegistry::RegisterPlugin(), and PluginRegistry::RegisterExternalApi().

However, in the inspected upstream header, the export/import macro is applied only to the free function GetPluginRegistry(). The PluginRegistry class and its member functions do not carry __declspec(dllexport) or __declspec(dllimport).

The documented example therefore does not establish a linkable ABI for a separate component DLL. Calling these non-virtual member functions from another component is not safe to assume will link or load correctly.

Inspected upstream source:

- https://github.com/NereaFantasia/foo_ui_webview2/blob/main/src/api/PluginRegistry.h
- https://github.com/NereaFantasia/foo_ui_webview2/blob/main/src/api/PluginRegistry.cpp
- https://github.com/NereaFantasia/foo_ui_webview2/blob/main/src/core/PanelBootstrap.cpp

## Required upstream contract

Before enabling crayonshell.getFileCreatedTime, foo_ui_webview2 needs a supported binary interface for third-party component registration.

Preferred direction: a small C ABI surface using opaque context and UTF-8 JSON strings, for example:

- Register a plugin namespace and one or more method callbacks.
- Invoke callbacks with a UTF-8 JSON parameter string.
- Receive a UTF-8 JSON result string.
- Unregister the plugin namespace when the component shuts down.

This avoids passing STL containers, std::function, or nlohmann::json objects across DLL boundaries.

The final signature must be agreed with the foo_ui_webview2 maintainer and exported by that component. This repository does not emulate or reach into PluginRegistry internals.

## Component-side selection service

The component now has an internal function that accepts a playlist index and item index, requires the foobar2000 main thread, and resolves the file path through playlist_manager::playlist_get_item_handle(). It then reads the physical file creation timestamp with GetFileAttributesExW.

The WebView API must not accept a path supplied by JavaScript. It must use the playlist coordinates to resolve the handle inside foobar2000.

For CUE and other multi-subsong inputs, the returned metadb_handle keeps the selected subsong identity while get_path() provides the physical file path used for filesystem metadata.

## Remaining work after the ABI is available

1. Register crayonshell.getFileCreatedTime from the component lifecycle.
2. Parse and validate playlistIndex and itemIndex in the JSON handler.
3. Dispatch playlist access onto the foobar2000 main thread if the WebView API callback is not already there.
4. Return a structured JSON response with Unix milliseconds or a Win32 error.
5. Test the real component with foo_ui_webview2 installed in foobar2000.

Until that ABI is available, the component binary is a scaffold and does not expose a browser API.
