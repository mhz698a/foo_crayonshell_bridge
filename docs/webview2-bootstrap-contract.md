# WebView2 local-service descriptor handoff

## Purpose

CrayonShell needs the ephemeral loopback port and per-process bearer token before it can call the local HTTP metadata API. The token must not be exposed by an unauthenticated HTTP endpoint, included in a URL, persisted in browser storage or written to logs.

No new host API is required. The integration uses the existing trusted WebView2 bridge and its existing filesystem policy.

## Existing host APIs used by CrayonShell

The frontend obtains the native foobar2000 profile directory with:

\`\`\`js
const profile = await window.fb2k.invoke("misc.getProfilePath", {});
const profilePath = profile.path || profile.value;
\`\`\`

It reads only the fixed descriptor file through the host's existing \`file.read\` method:

\`\`\`js
const file = await window.fb2k.invoke("file.read", {
  path: profilePath + "\\foo_crayonshell_bridge\\service.json"
});
const descriptor = JSON.parse(file.content);
\`\`\`

The actual frontend normalizes native Windows and \`file://\` profile path forms before joining the fixed relative path. The host's \`file.read\` uses the \`Read\` path-security policy, whose whitelist explicitly includes the foobar2000 profile. WebView2 bridge messages are already rejected when the top-level document or message source is not trusted.

## Descriptor location and schema

The Bridge publishes:

\`<foobar2000-profile>\\foo_crayonshell_bridge\\service.json\`

Example:

\`\`\`json
{
  "success": true,
  "service": "crayonshell",
  "protocolVersion": 1,
  "pid": 12345,
  "baseUrl": "http://127.0.0.1:49152",
  "token": "base64url-encoded-random-token",
  "instanceId": "base64url-encoded-256-bit-instance-id",
  "expiresAt": 1795600000000
}
\`\`\`

The descriptor is a small UTF-8 JSON document. The publisher rejects invalid ports and tokens and generates a fresh 256-bit instance ID for each component start.

## Bridge responsibilities

- Bind the HTTP listener exclusively to IPv4 loopback with an ephemeral port.
- Generate a fresh 256-bit CNG bearer token for each service start.
- Publish atomically to the fixed profile-relative location after the HTTP listener is running.
- Protect the component directory and descriptor file with a protected DACL granting full control only to the current Windows user and LocalSystem.
- Include protocol version, process ID, opaque instance ID, port, token and expiry.
- Keep the expiry at 24 hours and refresh it every six hours without rotating the running token or instance ID.
- Remove the descriptor before listener shutdown, but only when the stored PID and instance ID still match this instance.
- Never expose the descriptor over HTTP or log its token.

## Browser responsibilities

- Resolve the profile directory through \`misc.getProfilePath\`; do not accept a descriptor path from user input.
- Read only \`foo_crayonshell_bridge\\service.json\` through \`file.read\`.
- Retry briefly when the descriptor is not present yet during Bridge startup.
- Validate the JSON, service name, protocol version, positive PID, 256-bit token and instance ID, future expiry and exact \`http://127.0.0.1:<port>\` URL.
- Cache the descriptor only in memory.
- Send the token only as \`Authorization: Bearer <token>\`.
- Refresh the descriptor once after an authentication failure or connection refusal. Do not blindly retry non-idempotent writes; the current Properties inspector uses GET only.

## CORS and Local Network Access

The HTTP service allows only the exact production origin and the explicitly enabled Vite development origin. It does not use wildcard CORS.

The installed WebView2 runtime must be checked for:

- HTTPS trusted origin calling the IPv4 loopback HTTP endpoint.
- Standard \`OPTIONS\` preflight for \`Authorization\` and \`Content-Type\`.
- The exact \`Origin\` emitted by the embedded WebView2 document.
- Development mode at \`http://localhost:5173\`.
- Whether Chromium Local Network Access (LNA) is enabled and requires loopback permission.

Do not add the deprecated Private Network Access \`Access-Control-Allow-Private-Network\` header as a replacement for LNA permission. Current Chromium guidance uses browser-side local/loopback network permissions together with standard CORS. Do not weaken the host's global SSRF protections.

## Acceptance criteria

1. A trusted CrayonShell page obtains the descriptor through the existing host APIs while the Bridge is running.
2. An untrusted document cannot call the host bridge because the existing WebView2 origin gate rejects it.
3. The fixed descriptor file is readable through the host's profile-scoped \`Read\` policy and is not exposed through HTTP.
4. A component restart rotates the token and instance ID; the frontend reads the new descriptor.
5. Orderly component shutdown removes only the descriptor owned by that instance.
6. Production/dev CORS and the installed WebView2 runtime's LNA behavior are verified in foobar2000.
