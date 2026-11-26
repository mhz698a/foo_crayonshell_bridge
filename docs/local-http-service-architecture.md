# Local HTTP service architecture

## Decision

The component exposes a narrow, versioned JSON API through an HTTP/1.1 service bound exclusively to IPv4 loopback. It does not use `foo_ui_webview2::PluginRegistry` as its primary transport.

The service remains inside the foobar2000 process. The `initquit::on_init()` callback creates the controller and schedules `HttpServer::start()` on a dedicated startup thread; Winsock startup, socket binding, token generation, and worker/listener thread creation do not run inside foobar2000's UI-thread initialization callback. Shutdown first prevents new API work, joins any in-progress startup operation, then stops the server and joins its owned threads. Listener, parsing, authentication, request validation and Windows file metadata access run outside the foobar2000 UI thread. Only SDK calls that require the main thread are marshalled through the SDK callback queue.

No arbitrary filesystem paths are accepted from the frontend. Playlist coordinates are resolved by foobar2000 and the physical path is copied internally; only the copied path is used for Windows attribute access.

## Current implementation baseline

### Filesystem timestamps

- Reads CreationTime, LastAccessTime, LastWriteTime and Windows ChangeTime through `GetFileInformationByHandleEx(FileBasicInfo)`.
- Resolves a playlist row on the main thread into a retained `metadb_handle_ptr` and copied physical path.
- Preserves the exact CUE/subsong identity in the retained handle.
- Writes CreationTime, LastAccessTime and LastWriteTime through `SetFileTime`; ChangeTime writes are rejected.
- Reads actual values back after writes and reports write success separately from readback success.
- Converts FILETIME to Unix milliseconds and validates reverse conversion against signed `LARGE_INTEGER` range.

### Transport foundation — Phase B

The server is implemented in `src/http_server.*` and uses the strict incremental parser in `src/http_request_parser.*`:

- IPv4 loopback only (`127.0.0.1`), ephemeral port and `SO_EXCLUSIVEADDRUSE`.
- Random 256-bit per-instance bearer token generated through Windows CNG.
- Exact production origin allowlist and opt-in Vite development origin.
- Host, peer address, Origin and bearer token validation.
- Four bounded workers and a queue capped at sixteen accepted sockets by default.
- Request size, response size, socket timeout and total receive deadline limits.
- One request per connection, explicit HTTP/1.1, no keep-alive, no upgrade and no CONNECT.
- Accepted sockets are explicitly restored to blocking mode; receive/send timeouts are applied after accept.
- Orderly listener/worker shutdown, with queued and active sockets tracked.

GitHub Actions runs #112 and #113 completed successfully on the same transport commit. Both built the component for Win32 and x64, built and executed both test binaries, packaged the universal component, and uploaded the artifact. Server tests cover authentication, origin and Host validation, preflight policy, invalid tokens, Vite opt-in, queue saturation, slow-client deadlines, stop and restart.

The parser dependency is pinned to `nodejs/llhttp` release `release/v9.4.4`, commit prefix `8164852`, under MIT. CI stages generated C parser sources and license; no Node.js/npm/code generation is needed by the MSVC build.

## Phase C — read-only API

The first endpoints are:

- `GET /api/v1/system/health`: protocol/component version, uptime and implemented capability flags.
- `GET /api/v1/playlists/{playlistIndex}/items/{itemIndex}/timestamps`: resolve a playlist item through the SDK and return its filesystem timestamps.

The HTTP worker asks foobar2000's main-thread callback queue to resolve playlist coordinates. That callback performs only SDK work and copies the physical path; it never opens the file. The worker then calls the Windows file-attribute helper outside the UI thread. The route never returns a physical path or database internals.

The API is read-only in this phase. `PATCH` is not implemented; all write capability flags remain false. `metadbRefresh` also remains false: timestamp reads do not require a metadata refresh, and no database files are edited.

The SDK handoff has a bounded wait. A timed-out callback is marked cancelled and becomes a no-op if it is later dequeued. Shutdown prevents new dispatches and stops the listener before releasing the controller.

## Frontend origin and bootstrap dependency

CrayonShell's production origin is `https://foo-ui-webview2.local/`; Vite development uses `http://localhost:5173`. The exact allowed origins are configured in the transport. No wildcard CORS is used.

The component's port and bearer token are not exposed by an unauthenticated HTTP endpoint and are never written to logs. A narrowly scoped host bootstrap operation is still required to deliver the running descriptor to the trusted CrayonShell document. It must be coordinated with `foo_ui_webview2`; this integration is not part of Phase C.

Private Network Access behavior must be verified inside the actual embedded WebView2 runtime before adding any PNA allow response header. The current preflight behavior does not claim PNA compatibility.

## foobar2000 metadb rules

- Never edit foobar2000 database files or SQLite storage directly.
- Keep filesystem metadata operations separate from metadb refresh outcomes.
- Do not trigger a full tag reload merely because a timestamp was read or written.
- Select and test an SDK-supported update path only if Phase D demonstrates that a timestamp write needs a metadb notification.

## Remaining delivery phases

### Phase D — timestamp writes and metadb coordination

- Validate and enable write fields individually.
- Execute Windows attribute writes outside the UI thread.
- Read actual values back and report per-field outcomes.
- Determine experimentally whether a metadb notification is required; add it only through supported SDK interfaces.
- Add bounded asynchronous write jobs and queued-only cancellation.

### Phase E — CrayonShell integration and hardening

- Add the trusted WebView2 host bootstrap for port/token discovery.
- Add the typed React client behind the existing FoobarBridge boundary.
- Handle server restart, token rotation, missing component, timeout and version mismatch.
- Verify production/dev CORS and PNA in the embedded WebView2 runtime.
- Test the installed x86 component inside foobar2000 and validate x64 as well.

## Explicit non-goals

- No PluginRegistry dependency for the primary transport.
- No direct metadb/database edits.
- No arbitrary path access from JavaScript.
- No LAN exposure or wildcard socket binding.
- No unbounded thread or connection queue.
- No claim that Windows ChangeTime is writable.
- No synchronous file I/O or playlist enumeration on the foobar2000 UI thread.
