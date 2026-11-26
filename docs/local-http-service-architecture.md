# Local HTTP service architecture

## Decision

The component will expose its capabilities through a loopback-only HTTP/1.1 service. It will not use `foo_ui_webview2::PluginRegistry` as its primary transport.

The service remains inside the foobar2000 process, but its listener, HTTP parsing, authentication, request validation, and filesystem work must never run on the foobar2000 UI thread. Operations that require foobar2000 SDK objects are dispatched to the SDK-required thread.

This is a local service, not a general-purpose web server. It will expose a small, versioned JSON API and will not serve arbitrary files.

## Current implementation baseline

The filesystem timestamp helper now has a first implementation independent of HTTP:

- Reads CreationTime, LastAccessTime, LastWriteTime, and Windows ChangeTime through `GetFileInformationByHandleEx(FileBasicInfo)`.
- Writes CreationTime, LastAccessTime, and LastWriteTime through `SetFileTime`.
- Rejects ChangeTime writes with `ERROR_NOT_SUPPORTED`.
- Reads values back from the same file handle after a successful write and reports write success separately from readback success.
- Converts FILETIME to Unix milliseconds and validates the reverse conversion against the signed `LARGE_INTEGER` range used by `FILE_BASIC_INFO`.

This helper is not yet exposed to JavaScript and is not yet called by an HTTP route. Its Windows tests have been expanded, but the updated source has not yet been run through the Windows CI pipeline.

## Constraints discovered

### Frontend origin and existing host bridge

CrayonShell is loaded by `foo_ui_webview2` from the trusted origin `https://foo-ui-webview2.local/` in production and from `http://localhost:5173` during Vite development. The frontend already communicates with the host through `window.fb2k.invoke/on/off`; the React-side adapter is `src/services/foobarApi.js`.

A browser page cannot safely discover a random port and bearer token from a component-private file. The service therefore needs an explicit bootstrap/discovery path. The preferred approach is a narrowly scoped built-in host bootstrap method (not PluginRegistry registration) that returns the running service descriptor to the trusted CrayonShell document. The component writes/owns the descriptor and the host reads it, or an equivalent host-to-component bootstrap contract is agreed. This dependency must be resolved before enabling authenticated browser requests.

The production origin and development origin must be exact allowlist entries. No wildcard CORS is permitted. Chromium's private-network access behavior and the WebView2 host's request policy must be tested in the actual embedded runtime; ordinary browser assumptions are not sufficient.

### HTTP library assessment

`cpp-httplib` was evaluated and rejected for this component. Its upstream README explicitly says 32-bit platforms are not supported and that x86 receives only basic compile checks, not functional or security testing. This project must ship both x86 and x64 binaries, and x86 is the user's current foobar2000 host.

The selected direction is a small native Winsock transport with the maintained, pure-C `llhttp` parser. The parser is incremental, supports HTTP/1.x framing including chunked transfer encoding, and rejects ambiguous transfer-encoding/content-length combinations by default. Its generated C sources can be pinned and compiled by the existing MSVC x86/x64 workflow; the build must not require Node.js or code generation at build time.

The component will own the socket lifecycle and bounded worker scheduling. It will not implement HTTP request parsing by hand. Dependency source, exact upstream release/commit, license, and generated-source provenance must be pinned in the repository before production code is added.

## Process and thread model

1. **Component lifecycle (foobar2000 main thread):** initialize configuration, create the service controller, start the listener asynchronously, and register shutdown.
2. **Listener thread:** bind only to IPv4 loopback (`127.0.0.1`), accept connections, and enqueue accepted sockets. It does not call foobar2000 SDK APIs.
3. **Bounded HTTP workers:** a small fixed pool parses one request per connection, validates method/path/headers/body/authentication/origin, dispatches the request, writes the response, and closes the connection. Initial limits: 4 workers, 16 queued connections, 16 KiB request headers, 16 KiB JSON body, 3-second read timeout, and no keep-alive. Values are constants/configuration, not unbounded defaults.
4. **Filesystem workers:** perform Windows file-handle operations and timestamp reads/writes. They never access playlist manager or other thread-affine SDK services.
5. **SDK dispatcher:** executes only the SDK operations that require the foobar2000 main thread. HTTP workers submit bounded tasks and wait only for a short, explicit deadline; long operations return a job ID instead of blocking a connection.
6. **Job manager:** stores bounded job state and result summaries. Jobs have queued/running/completed/failed/cancelled states and expire after a configured retention period.

No detached threads. Shutdown stops accepting, closes queued sockets, cancels work that has not started, waits for workers with a bounded timeout, and prevents callbacks into destroyed component state.

## Binding and discovery

- Bind only to `127.0.0.1`; never bind to `0.0.0.0`, LAN addresses, or IPv6 wildcard.
- Obtain an available ephemeral port using an exclusive Winsock bind. Use `SO_EXCLUSIVEADDRUSE` on Windows to prevent another process from sharing the listener.
- Generate a cryptographically random per-process bearer token using the Windows CNG API (`BCryptGenRandom`).
- Publish a descriptor containing protocol version, port, token, process instance ID, and expiry. Store it under the current user's LocalAppData with an ACL limited to that user.
- The trusted WebView2 host bootstrap must deliver the descriptor only to the exact CrayonShell origin. Never expose the descriptor through an unauthenticated HTTP endpoint.
- Rotate the token on every component start and invalidate it during shutdown.
- Reject requests with an unexpected `Origin`, `Host`, or remote address. Allow only the production origin and the explicit Vite development origin when development mode is enabled.
- Implement exact-origin CORS and the required Private Network Access preflight response only after verifying the actual WebView2 behavior. CORS is not authentication; the bearer token remains mandatory.
- Do not accept credentials in URLs, query strings, logs, or error messages.
- The service is HTTP on loopback, not TLS. The random token, loopback binding, strict origin validation, and short lifetime are the local trust boundary; no claim of protection against another process running as the same Windows user is made.

## Request and API rules

- Prefix every endpoint with `/api/v1`.
- JSON is UTF-8; timestamps are Unix milliseconds on the wire and Windows `FILETIME` internally.
- Require `Authorization: Bearer <token>` on every API route except a minimal liveness probe, if one is retained at all.
- Reject unknown routes, unsupported methods, oversized bodies, duplicate/conflicting framing headers, malformed UTF-8, invalid JSON, and unexpected content types.
- Do not accept arbitrary filesystem paths from the frontend. File operations must resolve an opaque file ID or playlist coordinates through component-owned state and explicit authorization.
- No static file serving, directory listing, shell execution, arbitrary process launch, or generic file read/write endpoint.
- Use stable error codes and never leak local paths unless the relevant API explicitly needs a path and the caller is authorized.

## Timestamp model

Expose Windows concepts with explicit names; do not overload POSIX `ctime`.

- `creationTime`: Windows creation time. Read/write through supported Windows file-handle APIs.
- `lastAccessTime`: Windows last-access time. Read/write, subject to filesystem and OS update behavior.
- `lastWriteTime`: Windows last-write time. Read/write.
- `changeTime`: Windows change time. Read-only unless a supported, tested filesystem-specific mechanism is proven. Do not promise writes through `SetFileTime`, which does not expose a change-time argument.
- Preserve raw FILETIME precision internally; expose Unix milliseconds as the stable first API representation.
- Open handles with the minimum required access and sharing flags. Do not open media for data access when only metadata access is required.
- After a write, re-read the file metadata and report the actual persisted values, not merely the requested values.
- Return per-field status so an unsupported or denied field does not make unrelated successful fields ambiguous.

## foobar2000 metadb rules

The component must never edit foobar2000's database files or SQLite storage directly.

Filesystem timestamp changes and foobar2000 metadata refresh are separate operations and separate outcomes:

1. Resolve the selected item to a `metadb_handle` on the SDK-required thread.
2. Perform the filesystem operation on a worker using the resolved physical path/authorized file identity.
3. Revalidate the file handle/path and read back the resulting timestamps.
4. Request the appropriate SDK-supported metadata reload/update operation. Select between `metadb_io_v2::load_info_async`, `metadb_hint_list` and other SDK facilities only after checking their exact semantics in the pinned SDK headers.
5. Report filesystem success independently from metadb refresh completion. If refresh is asynchronous, return a job ID and expose its final status through the jobs API.

A timestamp-only change does not necessarily require reparsing audio tags. The implementation must not trigger a full metadata reload for every timestamp edit without demonstrating that foobar2000 needs it. The SDK operation and expected effect must be verified against the pinned SDK version.

## Initial endpoint contract

The first API slice is deliberately narrow:

- `GET /api/v1/system/health`: service version, protocol version, uptime, and capability flags; authenticated.
- `GET /api/v1/playlists/{playlistIndex}/items/{itemIndex}/timestamps`: resolve the item through foobar2000 and read supported timestamps.
- `PATCH /api/v1/playlists/{playlistIndex}/items/{itemIndex}/timestamps`: accept only explicitly supported timestamp fields; execute file writes asynchronously where appropriate.
- `GET /api/v1/jobs/{jobId}`: query a bounded asynchronous operation.
- `DELETE /api/v1/jobs/{jobId}`: request cancellation only while a job has not entered a non-cancellable OS/SDK operation.

The API will not initially expose generic file IDs, arbitrary paths, metadata editing, or directory scanning. Those require separate authorization and consistency designs.

## Delivery phases

### Phase A — contract and bootstrap
- Pin the HTTP parser source and license.
- Specify the Winsock listener, parser adapter, worker queue, and shutdown ownership.
- Determine the exact WebView2 bootstrap mechanism for port/token discovery.
- Verify CORS and Private Network Access in production and development WebView2 origins.

### Phase B — transport foundation
- Implement loopback listener, ephemeral exclusive port, token generation, bounded queue, bounded workers, request limits, origin/host checks, and graceful shutdown.
- Add parser, framing, malformed request, slow-client, queue saturation, and shutdown tests.
- Do not connect SDK or filesystem handlers yet.

### Phase C — read-only API
- Add health and playlist timestamp GET.
- Marshal playlist lookup to the SDK-required thread.
- Perform file metadata reads off the UI thread.
- Test x86 and x64 inside actual foobar2000.

### Phase D — timestamp writes and metadb coordination
- Add per-field write validation and Windows handle operations.
- Read back actual timestamps.
- Integrate only the SDK metadata refresh operation proven necessary.
- Add asynchronous jobs and cancellation semantics.

### Phase E — CrayonShell integration and hardening
- Add a typed React service client behind the existing FoobarBridge boundary.
- Handle server restart, token rotation, missing component, timeout, and version mismatch.
- Test WebView2 CORS/PNA behavior, origin rejection, local browser attacks, and x86/x64 runtime behavior.

## Explicit non-goals

- No PluginRegistry dependency for the primary API transport.
- No direct database edits.
- No arbitrary path operations from JavaScript.
- No network/LAN exposure.
- No unbounded thread pool or unbounded job/connection queue.
- No claim that Windows change time is writable.
- No synchronous disk or long-running work on foobar2000's UI thread.
