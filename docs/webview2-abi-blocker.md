# foo_ui_webview2 external API: Phase 2 ABI audit

## Audit result

**Status: the documented C++ registration example is not a dependable external-DLL contract.**

The current upstream source exposes an accessor named GetPluginRegistry(), but the export/import macro is applied to that free function only. The PluginRegistry class and its out-of-line member functions (RegisterPlugin, RegisterExternalApi, and others) are not declared with WEBVIEW_API.

The upstream project file defines FOO_UI_WEBVIEW2_EXPORTS, which makes WEBVIEW_API expand to __declspec(dllexport) while building foo_ui_webview2. That does not export class methods unless the class or individual methods are annotated. The current header annotates only GetPluginRegistry().

Consequently, obtaining a PluginRegistry& from the exported accessor does not by itself make calls to its non-exported member functions linkable from another DLL. The documented example demonstrates intended usage, but the header does not provide the required exported symbols for that usage.

There is a second, independent ABI concern: the documented C++ interface passes std::string, std::function, and nlohmann::json objects across the DLL boundary. That couples both components to compatible compiler, standard-library, runtime-library, and JSON-library ABI details. Exporting the class would address symbol visibility but would not remove this coupling.

## Upstream source inspected

The audit was performed against the current upstream main branch:

- src/api/PluginRegistry.h: export macro and public declarations.
- src/api/PluginRegistry.cpp: out-of-line definitions for registration methods.
- src/api/BridgeCore.cpp: bridge dispatch and thread behavior.
- foo_ui_webview2.vcxproj: DLL build configuration and export macro definition.

Relevant source links:

- https://github.com/NereaFantasia/foo_ui_webview2/blob/main/src/api/PluginRegistry.h
- https://github.com/NereaFantasia/foo_ui_webview2/blob/main/src/api/PluginRegistry.cpp
- https://github.com/NereaFantasia/foo_ui_webview2/blob/main/src/api/BridgeCore.cpp
- https://github.com/NereaFantasia/foo_ui_webview2/blob/main/foo_ui_webview2.vcxproj
- https://nereafantasia.github.io/foo_ui_webview2/sdk/system.html

The public documentation confirms that the plugin registry is intended to support third-party namespaces and that those APIs are discoverable through fb.system. This is useful evidence of intended product behavior, but does not replace a verifiable binary contract.

## Threading finding

The current BridgeCore::HandleMessage implementation sends synchronous API handlers through fb2k::inMainThread(...) before calling DispatchApiCall. Therefore, with the current bridge implementation, an external synchronous handler is invoked on foobar2000's main thread.

That matches the requirement of this component's playlist lookup, which calls playlist_manager and validates core_api::is_main_thread().

This is an implementation detail, not a promise of a future C ABI unless the upstream contract explicitly documents it. The ABI proposal below requires synchronous callbacks to run on the foobar2000 main thread. If foo_ui_webview2 later changes that guarantee, it must either preserve the guarantee for external callbacks or provide an explicit main-thread dispatch facility.

## Recommended upstream change: versioned C ABI

Do not export the C++ PluginRegistry class as the long-term integration contract. Keep its implementation private to foo_ui_webview2 and expose a small, versioned C ABI that adapts to the existing registry internally.

A suitable shape is one exported function returning a versioned function table:

    struct FooWebView2ExternalApiV1;

    typedef int32_t (__cdecl *FooWebView2ExternalHandlerV1)(
        void* user_data,
        const char* params_json_utf8,
        uint32_t params_size,
        char* result_json_utf8,
        uint32_t result_capacity,
        uint32_t* result_size
    );

    struct FooWebView2ExternalApiV1 {
        uint32_t abi_version;
        uint32_t struct_size;

        int32_t (__cdecl *register_plugin)(
            const char* plugin_namespace,
            const char* display_name,
            const char* version,
            const char* author,
            const char* description,
            void** plugin_handle
        );

        int32_t (__cdecl *register_method)(
            void* plugin_handle,
            const char* method_name,
            const char* description,
            FooWebView2ExternalHandlerV1 handler,
            void* user_data
        );

        int32_t (__cdecl *unregister_plugin)(void* plugin_handle);
    };

    extern "C" __declspec(dllexport)
    const FooWebView2ExternalApiV1* __cdecl
    FooWebView2_GetExternalApiV1();

This is a proposed contract, not an existing upstream API. The exact names and signatures should be agreed with the maintainer before implementation.

### Contract requirements

1. C-compatible boundary: only fixed-width integers, pointers, UTF-8 byte strings, lengths, function pointers, and opaque handles cross the boundary. No STL, exceptions, C++ classes, std::function, or JSON-library objects cross it.
2. Versioning: the exported getter returns a stable table with abi_version and struct_size. Future versions can append fields without silently changing the v1 layout.
3. Ownership: plugin handles are opaque and owned by foo_ui_webview2. Registration copies all metadata strings. The callback's input buffer is borrowed for the duration of the call. The host owns the output buffer and passes its capacity; the callback reports the required output length when the buffer is too small.
4. Callback status: define fixed status codes, including success, invalid arguments, insufficient output buffer, and handler failure. The host converts failures into the bridge's normal error envelope.
5. Threading: v1 callbacks are synchronous and are invoked on foobar2000's main thread. Registration and unregistration are also performed on that thread. The host catches all C++ exceptions before they can cross the C ABI.
6. Lifetime: after unregister_plugin returns, the host must not call that plugin's callbacks again. The plugin must keep callback code and user_data alive until unregistration completes.
7. No load-time dependency: foo_crayonshell_bridge should resolve the exported getter dynamically from the loaded foo_ui_webview2 module, rather than linking against an import library that would prevent the bridge from loading when foo_ui_webview2 is absent. If the module or ABI is unavailable, the bridge remains inert and foobar2000 continues normally.

The host-side adapter can translate between this C ABI and its private PluginRegistry::RegisterExternalApi implementation. All std::function, std::string, and nlohmann::json use remains inside foo_ui_webview2.

## Why not simply export the C++ class?

Adding WEBVIEW_API to class PluginRegistry (or to each member function) would likely make the documented calls linkable for a tightly controlled build. It would still expose C++ library types and callback objects across independently built component DLLs. That is a short-term compatibility patch, not a stable plugin ABI.

## Integration plan after upstream agreement

1. Confirm the ABI table and callback contract with the foo_ui_webview2 maintainer.
2. Wait for an upstream release that exports and documents the agreed ABI.
3. Add a small local ABI header to this repository containing only the agreed C declarations.
4. Resolve the ABI getter dynamically and validate its version and structure size.
5. Register the crayonshell namespace and getFileCreatedTime method during component initialization.
6. Parse playlist coordinates from the incoming JSON, validate them, and call the existing playlist-handle service.
7. Return a JSON result containing Unix milliseconds or a stable error code.
8. Unregister the namespace during component shutdown.
9. Test with foo_ui_webview2 both present and absent, on foobar2000 x86 and x64.

## Component-side selection service

The component already has an internal function that accepts a playlist index and item index, requires the foobar2000 main thread, and resolves the file path through playlist_manager::playlist_get_item_handle(). It then reads the physical file creation timestamp with GetFileAttributesExW.

The WebView API must not accept a path supplied by JavaScript. It must use playlist coordinates to resolve the handle inside foobar2000.

For CUE and other multi-subsong inputs, the returned metadb_handle retains the selected subsong identity while get_path() provides the physical file path used for filesystem metadata.

Until an upstream binary ABI is available and verified, this repository remains a buildable x86/x64 scaffold and does not expose a browser API.
