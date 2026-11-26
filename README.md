# foo_crayonshell_bridge

A Windows x86 and x64 foobar2000 component being developed to expose narrowly scoped file metadata to CrayonShell.

## Current status

The initial scaffold contains:

- A read-only FILETIME query using GetFileAttributesExW.
- Conversion from Windows FILETIME ticks to Unix milliseconds.
- A playlist-index lookup that retrieves the foobar2000 metadb_handle and resolves its physical path on the main thread.
- Windows unit tests for epoch conversion, existing files, missing files, empty paths, and directories.
- A GitHub Actions build that compiles and tests both Win32 (x86) and x64, then packages both binaries in one .fb2k-component.

**The current binary is not yet functionally integrated with CrayonShell.** It does not expose crayonshell.getFileCreatedTime until foo_ui_webview2 provides a supported external-plugin ABI. See the ABI prerequisite in docs/webview2-abi-blocker.md.

## Build pipeline

The workflow uses:

- Windows Server 2022 runner.
- MSVC v143 / C++20.
- Official foobar2000 SDK snapshot SDK-2026-10-01, pinned to commit 9e4823ee802dfb704de1ce42401b48b407a54180.
- Release builds and test runs for Win32 (x86) and x64.

On pushes and pull requests, GitHub Actions builds both DLLs, runs both test executables, creates a multi-architecture .fb2k-component, and uploads the package and SHA-256 file as an Actions artifact.

When a version tag such as v0.1.0 is pushed, the workflow also creates or updates a GitHub Release with the same multi-architecture package and its SHA-256 file.

## Multi-architecture package layout

The package is named:

foo_crayonshell_bridge-<version>-x86-x64.fb2k-component

It contains:

- foo_crayonshell_bridge.dll at the archive root: legacy 32-bit x86 build.
- x64/foo_crayonshell_bridge.dll: 64-bit x64 build.

This follows foobar2000's multi-architecture component package convention. The same file can be installed in 32-bit or 64-bit foobar2000; each host uses its matching binary.

## Local build

Requirements: Windows, Visual Studio 2022 Build Tools or Visual Studio 2026 with the C++ desktop workload, PowerShell, and Git.

The workflow stages the SDK into vendor/. To reproduce locally:

1. Check out the pinned SDK snapshot from reupen/foobar2000-sdk-unmodified.
2. Copy foobar2000/SDK, foobar2000/helpers, foobar2000/shared, foobar2000/foobar2000_component_client, and pfc into the same relative structure under vendor/.
3. Build foo_crayonshell_bridge.vcxproj in Release / Win32.
4. Build foo_crayonshell_bridge.vcxproj in Release / x64.
5. Build and run tests/foo_crayonshell_bridge_tests.vcxproj in Release / Win32 and Release / x64.
6. Run scripts/package-component.ps1.

The package script requires both bin/Release_Win32/foo_crayonshell_bridge.dll and bin/Release_x64/foo_crayonshell_bridge.dll; it refuses to create a package if either architecture is missing.

The generated package and checksum are written to dist/.

## Scope

The component is read-only. It does not edit tags, artwork, or media contents. The WebView API will pass only playlist coordinates; it will never be allowed to submit an arbitrary filesystem path.
