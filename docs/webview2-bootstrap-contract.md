# WebView2 local-service bootstrap contract

## Problem

The CrayonShell document must know the HTTP service's ephemeral port and per-process bearer token before it can call the service. The browser must not use direct filesystem access. It obtains the descriptor through the host's existing trusted bridge and the narrowly scoped `file.read` API, which applies the host profile-path read policy. The HTTP server must not expose the token through an unauthenticated endpoint.

CrayonShell already uses the trusted `window.fb2k.invoke/on/off` bridge. The host injects that object into trusted pages, and bridge calls are dispatched by the native `foo_ui_webview2` backend. This existing bridge is the bootstrap channel; it is not the transport for the file operations themselves.

## Proposed host contract

Add one narrowly scoped built-in host method to `foo_ui_webview2`:

```js
const descriptor = await window.fb2k.invoke(
  "system.getLocalServiceDescriptor",
  { service: "crayonshell" }
);
```

Successful response:

```json
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
```

Failure response when the component is not installed or not running:

```json
{
  "success": false,
  "code": "SERVICE_UNAVAILABLE"
}
```

This is a proposed host API, not an existing `foo_ui_webview2` method.

## Host-side responsibilities

- Accept only the fixed service identifier `crayonshell`; do not accept a path, URL, port, or token from the page.
- Obtain the native profile path through the existing `misc.getProfilePath()` API.
- Read only `<profile>\\foo_crayonshell_bridge\\service.json` through the existing `file.read({ path })` API; never accept a path from the page.
- The host's existing message gate permits Bridge calls only from trusted document origins; the file API additionally applies its `Read` path policy, whose whitelist explicitly includes the foobar2000 profile.
- Reject reparse points, non-regular files, oversized JSON, malformed fields and non-loopback URLs.
- Verify the descriptor DACL grants access only to the current Windows user and LocalSystem.
- Require a positive `pid`, and verify service, protocol version, token shape, instance ID and expiry before returning it.
- Return the descriptor only when the calling document has the exact trusted production origin `https://foo-ui-webview2.local/` or the explicit development origin `http://localhost:5173` while development mode is enabled.
- Never expose the descriptor to arbitrary popup URLs, untrusted origins, or the built-in test page.
- Do not cache a token beyond its expiry. Re-read the current descriptor after service restarts.
- Return only the descriptor from the profile resolved by `misc.getProfilePath`. Do not enumerate other profiles or service instances.

## Component-side responsibilities

- Generate a fresh cryptographically random token for every service start.
- Bind the listener to `127.0.0.1` and an exclusively reserved ephemeral port.
- Publish atomically to `<foobar2000-profile>\\foo_crayonshell_bridge\\service.json`.
- Protect the component directory and descriptor file with a protected DACL granting full control only to the current user and LocalSystem.
- Include protocol version, process ID, opaque 256-bit instance ID, loopback port, 256-bit token, and expiry.
- Refresh the expiry every six hours without rotating the instance ID or bearer token; the descriptor lifetime is 24 hours.
- Remove or invalidate the descriptor before listener shutdown.
- Rotate the token on restart. Never log the token.
- Treat a missing, malformed, stale, or mismatched descriptor as unavailable.

The Bridge implementation defines the path and ACL above. CrayonShell discovers the profile using `misc.getProfilePath` and reads this fixed descriptor with `file.read`; no new host API or generic file-reading endpoint is introduced.

## Browser-side responsibilities

CrayonShell's FoobarBridge adapter obtains the descriptor through the existing host bridge and caches it only in memory. The HTTP client then sends:

- `Authorization: Bearer <token>`
- `Origin: https://foo-ui-webview2.local` in production (or the exact development origin)
- `Content-Type: application/json` for JSON writes

The adapter must refresh the descriptor once after an authentication failure or connection refusal, then fail with a typed service-unavailable error if the service is still unavailable. It must not retry non-idempotent writes blindly.

## CORS and Local Network Access

The HTTP service allows only the exact production origin and the explicitly enabled Vite development origin. It does not use wildcard CORS.

The implementation must verify in the installed WebView2 runtime:

- HTTPS trusted origin calling the IPv4 loopback HTTP endpoint.
- Standard `OPTIONS` preflight for `Authorization` and `Content-Type`.
- The exact `Origin` value emitted by the embedded WebView2 document.
- Development mode when the page origin is `http://localhost:5173`.
- Whether that WebView2 runtime enables Chromium Local Network Access (LNA), and whether its loopback permission is granted or prompts the user.

Do not add the deprecated Private Network Access `Access-Control-Allow-Private-Network` header as a substitute for LNA permission. Current Chromium guidance uses browser-side local/loopback network permissions alongside standard CORS. The page uses native WebView2 `fetch`; the host's generic HTTP API is not used because it blocks localhost/private destinations. Do not weaken the host's global SSRF protections.

## Acceptance criteria

1. A trusted CrayonShell page can obtain the descriptor while the component is running.
2. An untrusted page and a popup on another origin cannot obtain it.
3. The descriptor is not available from an HTTP endpoint without authentication.
4. A component restart rotates both instance ID and token; the frontend recovers without a foobar2000 restart.
5. When the component is absent or stopped, the host returns a stable `SERVICE_UNAVAILABLE` response.
6. The production and Vite development origins are tested in the actual WebView2 runtime.
