#include <SDK/foobar2000.h>

#include "version.h"

DECLARE_COMPONENT_VERSION(
    "CrayonShell File Properties Bridge",
    CRAYONSHELL_BRIDGE_VERSION,
    "Read-only filesystem metadata helper for CrayonShell.\n"
    "External WebView API registration is pending a stable foo_ui_webview2 ABI."
);

VALIDATE_COMPONENT_FILENAME("foo_crayonshell_bridge.dll");
