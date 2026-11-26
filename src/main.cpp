#include <SDK/foobar2000.h>

#include "version.h"

DECLARE_COMPONENT_VERSION(
    "CrayonShell File Properties Bridge",
    CRAYONSHELL_BRIDGE_VERSION,
    "Local file metadata service for CrayonShell.\n"
    "Loopback HTTP transport is under development; no remote listener is exposed."
);

VALIDATE_COMPONENT_FILENAME("foo_crayonshell_bridge.dll");
