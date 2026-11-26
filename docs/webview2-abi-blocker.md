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

## Component-side contract once the ABI exists

The browser API will accept a playlist index and item index, not an arbitrary path. The component will:

1. Validate both indices against the current playlist manager state.
2. Retrieve the metadb_handle with playlist_manager::playlist_get_item_handle() on the foobar2000 main thread.
3. Resolve the item's physical path from the handle.
4. Query WIN32_FILE_ATTRIBUTE_DATA.ftCreationTime with GetFileAttributesExW.
5. Convert the Windows FILETIME value to Unix milliseconds.
6. Return a structured JSON response, including a Win32 error on failure.

Until that ABI is available, this repository only builds and tests the Windows filesystem timestamp helper. The current component binary is a scaffold and does not yet expose a browser API.
