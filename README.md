# foo_crayonshell_bridge

A Windows x86 and x64 foobar2000 component that provides a narrowly scoped local service for CrayonShell file metadata operations.

## Current status

The repository currently contains the initial x86/x64 component scaffold and a tested multi-architecture build pipeline. The next implementation direction is a loopback-only HTTP/1.1 JSON service; `foo_ui_webview2::PluginRegistry` is no longer the planned primary transport.

Existing implementation:

- A Windows `FILE_BASIC_INFO` timestamp reader for creation, last-access, last-write, and change times.
- A `SetFileTime` writer for creation, last-access, and last-write times; Windows change time is explicitly rejected as unsupported.
- Conversion between Windows FILETIME ticks and Unix milliseconds, including overflow validation.
- Readback of actual timestamp values after writes, with write success and readback success reported separately.
- A playlist-index lookup that retrieves the foobar2000 `metadb_handle` and resolves its physical path on the SDK-required thread.
- Windows unit tests for timestamp conversions, file timestamp reads/writes, unsupported ChangeTime writes, missing files, empty paths, and directories.
- A strict, bounded HTTP/1.1 request parser adapter based on llhttp, with tests for incremental input, Content-Length, chunked bodies, framing conflicts, upgrades, and request limits.
- GitHub Actions builds and runs tests for Win32 (x86) and x64, then packages both binaries in one `.fb2k-component`.

**The HTTP service and Winsock listener are not implemented yet.** The bounded llhttp parser adapter is now integrated into the component and test project definitions. The architecture and first API contract are documented in:

- [Local HTTP service architecture](docs/local-http-service-architecture.md)
- [HTTP API v1 contract](docs/http-api-v1.md)
- [WebView2 bootstrap contract](docs/webview2-bootstrap-contract.md)
- [Historical foo_ui_webview2 ABI audit](docs/webview2-abi-blocker.md)

A key integration prerequisite remains: securely delivering the service's ephemeral port and per-instance bearer token from the trusted WebView2 host to the CrayonShell document. The production document origin is `https://foo-ui-webview2.local/`; Vite development uses `http://localhost:5173`. CORS/Private Network Access behavior must be verified in the embedded WebView2 runtime.

## Build pipeline

The workflow uses:

- Windows Server 2022 runner.
- MSVC v143 / C++20.
- Official foobar2000 SDK snapshot SDK-2026-10-01, pinned to commit `9e4823ee802dfb704de1ce42401b48b407a54180`.
- Release builds and test runs for Win32 (x86) and x64.

On pushes and pull requests, GitHub Actions builds both DLLs, runs both test executables, creates a multi-architecture `.fb2k-component`, and uploads the package and SHA-256 file as an Actions artifact.

When a version tag such as `v0.1.0` is pushed, the workflow also creates or updates a GitHub Release with the same multi-architecture package and its SHA-256 file.

## Multi-architecture package layout

The package is named:

`foo_crayonshell_bridge-<version>-x86-x64.fb2k-component`

It contains:

- `foo_crayonshell_bridge.dll` at the archive root: legacy 32-bit x86 build.
- `x64/foo_crayonshell_bridge.dll`: 64-bit x64 build.

This follows foobar2000's multi-architecture component package convention. The same file can be installed in 32-bit or 64-bit foobar2000; each host uses its matching binary.

## Local build

Requirements: Windows, Visual Studio 2022 Build Tools or Visual Studio 2026 with the C++ desktop workload, PowerShell, and Git.

The workflow stages the foobar2000 SDK and llhttp into `vendor/`. For llhttp, it checks out `nodejs/llhttp` at `release/v9.4.4`, verifies commit prefix `8164852`, stages the generated C sources and MIT license, then compiles the C sources with MSVC. No Node.js code generation is required. To reproduce locally:

1. Check out the pinned SDK snapshot from `reupen/foobar2000-sdk-unmodified`.
2. Copy `foobar2000/SDK`, `foobar2000/helpers`, `foobar2000/shared`, `foobar2000/foobar2000_component_client`, and `pfc` into the same relative structure under `vendor/`.
3. Check out `nodejs/llhttp` at `release/v9.4.4`; copy `include/llhttp.h`, `src/llhttp.c`, `src/api.c`, `src/http.c`, and `LICENSE` into `vendor/llhttp/` with the same include/src layout as the workflow.
4. Build `foo_crayonshell_bridge.vcxproj` in Release / Win32.
5. Build `foo_crayonshell_bridge.vcxproj` in Release / x64.
6. Build and run `tests/foo_crayonshell_bridge_tests.vcxproj` in Release / Win32 and Release / x64.
7. Run `scripts/package-component.ps1`.

The package script requires both `bin/Release_Win32/foo_crayonshell_bridge.dll` and `bin/Release_x64/foo_crayonshell_bridge.dll`; it refuses to create a package if either architecture is missing.

The generated package and checksum are written to `dist/`.

## Scope and safety

- The service will bind only to IPv4 loopback and will never be exposed to the LAN.
- The frontend will not be allowed to submit arbitrary filesystem paths.
- File operations will resolve foobar2000 playlist items or explicitly authorized opaque file references.
- Filesystem operations and HTTP handling will run outside the foobar2000 UI thread.
- foobar2000's metadata database will only be updated through supported SDK interfaces; database files will never be edited directly.
- Windows change time is read-only unless a supported write mechanism is verified.
