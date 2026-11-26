# foo_crayonshell_bridge

A Windows x86 and x64 foobar2000 component that provides a narrowly scoped local service for CrayonShell file metadata operations.

## Current implementation

The current feature branch contains:

- A loopback-only HTTP/1.1 service with a per-instance CNG bearer token, strict origin/host checks, bounded connection workers and orderly shutdown.
- A strict incremental llhttp-based request parser.
- GET /api/v1/system/health.
- GET /api/v1/playlists/{playlistIndex}/items/{itemIndex}/timestamps.
- PATCH /api/v1/playlists/{playlistIndex}/items/{itemIndex}/timestamps for Windows creation, last-access and last-write times.
- Asynchronous timestamp jobs with bounded workers/queue, opaque job IDs, status polling and queued-only cancellation.
- File size in bytes read from Windows `FileStandardInfo`, independently of foobar2000 Title Formatting.
- Windows ChangeTime reads; ChangeTime writes are explicitly unsupported.
- Readback of actual timestamp values after writes.
- x86 and x64 build/test/package automation.

The component does not accept arbitrary filesystem paths from JavaScript. It resolves a playlist row through foobar2000's SDK main-thread callback queue, captures the physical path internally, and performs Windows file-attribute operations on owned worker threads.

## Current limitations

- A trusted foo_ui_webview2 bootstrap is still required to deliver the local service port and bearer token to the trusted CrayonShell document. Until that bridge is implemented, the API cannot yet be called from CrayonShell.
- Production/dev CORS and Private Network Access behavior still need verification in the embedded WebView2 runtime.
- metadbRefresh remains disabled. Timestamp writes do not trigger speculative metadata reloads or direct database edits; an installed-host test must first establish whether any supported metadb notification is required.
- Runtime testing inside the user's installed x86 foobar2000 host remains required. CI validates Windows builds and automated tests, not the embedded host lifecycle.

## Build pipeline

The workflow uses Windows Server 2022, MSVC v143 / C++20, a pinned foobar2000 SDK snapshot and llhttp v9.4.4. It builds the component and test executable for Win32 (x86) and x64, runs both test suites, packages a multi-architecture .fb2k-component, and uploads the package plus SHA-256 checksum.

The component package contains the x86 DLL at the archive root and the x64 DLL under x64/. foobar2000 selects the matching binary for its architecture.

## Local build

Requirements: Windows, Visual Studio 2022 Build Tools or newer with the C++ desktop workload, PowerShell, and Git.

The workflow stages the foobar2000 SDK and llhttp into vendor/. For a local reproduction:

1. Check out the pinned SDK snapshot from reupen/foobar2000-sdk-unmodified.
2. Copy foobar2000/SDK, foobar2000/helpers, foobar2000/shared, foobar2000/foobar2000_component_client, and pfc into the same relative structure under vendor/.
3. Check out nodejs/llhttp at release/v9.4.4; copy include/llhttp.h, src/llhttp.c, src/api.c, src/http.c, and LICENSE into vendor/llhttp/.
4. Build foo_crayonshell_bridge.vcxproj in Release / Win32 and Release / x64.
5. Build and run tests/foo_crayonshell_bridge_tests.vcxproj in both architectures.
6. Run scripts/package-component.ps1.

The package and checksum are written to dist/.

## Security constraints

- The service binds only to 127.0.0.1; it is never exposed to the LAN.
- The frontend cannot supply arbitrary filesystem paths.
- HTTP and filesystem work remain off foobar2000's UI thread.
- SDK calls run only on the thread required by foobar2000.
- foobar2000 database files are never edited directly.
- Windows ChangeTime is read-only unless a supported write mechanism is independently verified.


## Trusted WebView2 bootstrap descriptor

After the loopback HTTP listener starts, the component writes its descriptor atomically to:

`<foobar2000-profile>\\foo_crayonshell_bridge\\service.json`

The component directory, PID directory and descriptor use a protected DACL that grants full control only to the current Windows user and LocalSystem. The descriptor contains the current process ID, loopback port, bearer token, 256-bit instance ID and expiry. It is removed before orderly listener shutdown; its 24-hour expiry is refreshed every six hours without rotating the running credentials.

The descriptor is not served by HTTP. The trusted `foo_ui_webview2` host still needs to implement `system.getLocalServiceDescriptor({ service: "crayonshell" })`, read only the current process's fixed descriptor path, verify the caller origin and return the descriptor to the approved page. Until that host method is available, CrayonShell cannot obtain the token and the timestamp inspector cannot complete its HTTP request.
