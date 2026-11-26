# WebView2 local-service bootstrap contract

## Problem

The CrayonShell document must know the HTTP service's ephemeral port and per-process bearer token before it can call the service. The browser must not read a component-private descriptor file directly, and the HTTP server must not expose the token through an unauthenticated endpoint.

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
- Read only `%LOCALAPPDATA%\\foobar2000\\foo_crayonshell_bridge\\<current-process-id>\\service.json`; never accept a path from the page.
- Reject reparse points, non-regular files, oversized JSON, malformed fields and non-loopback URLs.
- Verify the descriptor DACL grants access only to the current Windows user and LocalSystem.
- Require `pid` to equal `GetCurrentProcessId()`, and verify service, protocol version, token shape, instance ID and expiry before returning it.
- Return the descriptor only when the calling document has the exact trusted production origin `https://foo-ui-webview2.local/` or the explicit development origin `http://localhost:5173` while development mode is enabled.
- Never expose the descriptor to arbitrary popup URLs, untrusted origins, or the built-in test page.
- Do not cache a token beyond its expiry. Re-read the current descriptor after service restarts.
- Return only the descriptor for the current foobar2000 process. Do not enumerate other processes or service instances.

## Component-side responsibilities

- Generate a fresh cryptographically random token for every service start.
- Bind the listener to `127.0.0.1` and an exclusively reserved ephemeral port.
- Publish atomically to `%LOCALAPPDATA%\\foobar2000\\foo_crayonshell_bridge\\<pid>\\service.json`.
- Protect the component and process directories and descriptor file with a protected DACL granting full control only to the current user and LocalSystem.
- Include protocol version, process ID, opaque 256-bit instance ID, loopback port, 256-bit token, and expiry.
- Refresh the expiry every six hours without rotating the instance ID or bearer token; the descriptor lifetime is 24 hours.
- Remove or invalidate the descriptor before listener shutdown.
- Rotate the token on restart. Never log the token.
- Treat a missing, malformed, stale, or mismatched descriptor as unavailable.

The Bridge implementation defines the path and ACL above. The host consumes only this fixed file for the current process; it does not expose a generic file-reading API.

## Browser-side responsibilities

CrayonShell's FoobarBridge adapter obtains the descriptor through the existing host bridge and caches it only in memory. The HTTP client then sends:

- `Authorization: Bearer <token>`
- `Origin: https://foo-ui-webview2.local` in production (or the exact development origin)
- `Content-Type: application/json` for JSON writes

The adapter must refresh the descriptor once after an authentication failure or connection refusal, then fail with a typed service-unavailable error if the service is still unavailable. It must not retry non-idempotent writes blindly.

## CORS and Private Network Access

The server must allow only the exact production and explicitly enabled development origins. It must not use `Access-Control-Allow-Origin: *`.

The implementation must verify actual WebView2 behavior for:

- HTTPS trusted origin calling an HTTP loopback endpoint.
- Chromium Private Network Access preflight.
- `OPTIONS` handling for Authorization and JSON content type.
- The exact `Origin` value emitted by the embedded WebView2 document.
- Development mode when the page origin is `http://localhost:5173`.

The host's existing HTTP client API blocks localhost/private destinations by design, so the CrayonShell browser client should use the WebView2 page's native `fetch` only after the loopback/CORS/PNA behavior is verified. Do not weaken the host's global SSRF protections to make this service work.

## Acceptance criteria

1. A trusted CrayonShell page can obtain the descriptor while the component is running.
2. An untrusted page and a popup on another origin cannot obtain it.
3. The descriptor is not available from an HTTP endpoint without authentication.
4. A component restart rotates both instance ID and token; the frontend recovers without a foobar2000 restart.
5. When the component is absent or stopped, the host returns a stable `SERVICE_UNAVAILABLE` response.
6. The production and Vite development origins are tested in the actual WebView2 runtime.
