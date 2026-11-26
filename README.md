# foo_crayonshell_bridge

A Windows x64 foobar2000 component being developed to expose narrowly scoped file metadata to CrayonShell.

## Current status

The initial scaffold contains:

- A read-only helper for querying a regular file's filesystem creation timestamp.
- Conversion from Windows FILETIME ticks to Unix milliseconds.
- Windows unit tests for epoch conversion, existing files, missing files, empty paths, and directories.
- A GitHub Actions build that compiles the component, runs tests, and packages a .fb2k-component.

**The current binary is not yet functionally integrated with CrayonShell.** It does not expose crayonshell.getFileCreatedTime until foo_ui_webview2 provides a supported external-plugin ABI. See the ABI prerequisite in docs/webview2-abi-blocker.md.

## Build pipeline

The workflow uses:

- Windows Server 2022 runner.
- MSVC v143 / C++20.
- Official foobar2000 SDK snapshot SDK-2026-10-01, pinned to commit 9e4823ee802dfb704de1ce42401b48b407a54180.
- x64 Release configuration.

On pushes and pull requests, GitHub Actions builds the DLL, runs the helper tests, and uploads the install package as an Actions artifact.

When a version tag such as v0.1.0 is pushed, the workflow also creates a GitHub Release with the .fb2k-component package and its SHA-256 file.

## Local build

Requirements: Windows, Visual Studio 2022 Build Tools or Visual Studio 2026 with the C++ desktop workload, PowerShell, and Git.

The workflow stages the SDK into vendor/. To reproduce locally:

1. Check out the pinned SDK snapshot from reupen/foobar2000-sdk-unmodified.
2. Copy foobar2000/SDK, foobar2000/helpers, foobar2000/shared, foobar2000/foobar2000_component_client, and pfc into the same relative structure under vendor/.
3. Build foo_crayonshell_bridge.vcxproj and tests/foo_crayonshell_bridge_tests.vcxproj with MSBuild, configuration Release, platform x64.
4. Run bin/tests/Release_x64/foo_crayonshell_bridge_tests.exe.
5. Run scripts/package-component.ps1.

The generated package is written to dist/.

## Scope

The component is read-only. It does not edit tags, artwork, or media contents. It will use the foobar2000 playlist item handle as its authority and will not accept arbitrary filesystem paths from the WebView frontend.
