# foo_crayonshell_bridge

Native foobar2000 component that exposes narrowly scoped file metadata to CrayonShell.

## Goals

- Windows x64 component for foobar2000 2.0+.
- Expose the selected playlist item's filesystem creation timestamp to the CrayonShell WebView frontend.
- Keep the component read-only; it does not modify media files or tags.
- Build and package the installable `.fb2k-component` with GitHub Actions.

## Status

Initial repository bootstrap. The component API contract and the external API integration with `foo_ui_webview2` are being validated before implementation.

## Build

The intended build uses a pinned official foobar2000 SDK and a Windows GitHub Actions runner. Build instructions will be added with the first implementation.
