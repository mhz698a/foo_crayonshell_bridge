# Local HTTP service architecture

## Decision

The component exposes a narrow, versioned JSON API through an HTTP/1.1 service bound exclusively to IPv4 loopback. It does not use foo_ui_webview2::PluginRegistry as its primary transport.

The service remains inside the foobar2000 process. The initquit::on_init() callback creates the controller and schedules job-worker and HTTP-service startup on an owned startup thread. Winsock startup, socket binding, token generation, and worker/listener thread creation do not run inside the UI-thread initialization callback. Shutdown prevents new API work, joins any in-progress startup operation, stops HTTP handlers, and then joins the filesystem job workers. No detached threads are used.

No arbitrary filesystem paths are accepted from the frontend. Playlist coordinates are resolved by foobar2000 and the physical path is copied internally; only the copied path is used for Windows attribute access.

## Filesystem timestamp layer

- Reads file size through GetFileInformationByHandleEx(FileStandardInfo), and CreationTime, LastAccessTime, LastWriteTime and Windows ChangeTime through GetFileInformationByHandleEx(FileBasicInfo).
- Resolves a playlist row on the main thread into a retained metadb_handle_ptr and copied physical path.
- Preserves the exact CUE/subsong identity while the lookup result is retained.
- Writes CreationTime, LastAccessTime and LastWriteTime through SetFileTime; ChangeTime writes are rejected.
- Reads actual values back after writes and reports write success separately from readback success.
- Converts FILETIME to Unix milliseconds and validates reverse conversion against signed LARGE_INTEGER range.

## HTTP transport

The server is implemented in src/http_server.* and uses the strict incremental parser in src/http_request_parser.*:

- IPv4 loopback only (127.0.0.1), ephemeral port and SO_EXCLUSIVEADDRUSE.
- Random 256-bit per-instance bearer token generated through Windows CNG.
- Exact production origin allowlist and opt-in Vite development origin.
- Host, peer address, Origin and bearer token validation.
- Four bounded HTTP workers and a queue capped at sixteen accepted sockets by default.
- Request size, response size, socket timeout and total receive deadline limits.
- One request per connection, explicit HTTP/1.1, no keep-alive, no upgrade and no CONNECT.
- Accepted sockets are explicitly restored to blocking mode; receive/send timeouts are applied after accept.
- Orderly listener/worker shutdown, with queued and active sockets tracked.

The parser dependency is pinned to nodejs/llhttp release v9.4.4, commit prefix 8164852, under MIT. CI stages generated C parser sources and license; no Node.js/npm/code generation is needed by the MSVC build.

## Read API

- GET /api/v1/system/health: protocol/component version, uptime and implemented capability flags.
- GET /api/v1/playlists/{playlistIndex}/items/{itemIndex}/timestamps: resolve a playlist item through the SDK and return its filesystem size and timestamps.

The HTTP worker asks foobar2000's main-thread callback queue to resolve playlist coordinates. That callback performs only SDK work and copies the physical path; it never opens the file. The worker then calls the Windows file-attribute helper outside the UI thread. The route never returns a physical path or database internals.

## Write API and bounded jobs

- PATCH /api/v1/playlists/{playlistIndex}/items/{itemIndex}/timestamps accepts a strict JSON object containing only creationTime, lastAccessTime and/or lastWriteTime.
- Playlist lookup occurs before queue submission. Each job captures the internally resolved physical path so a later playlist reorder cannot redirect the write.
- The API returns 202 Accepted and a 128-bit CNG-generated job identifier.
- Two owned filesystem workers process jobs; at most sixteen jobs may wait in the queue.
- The manager retains at most 128 job records. Older terminal records are evicted only when the history limit is reached.
- GET /api/v1/jobs/{jobId} reports state, write result, readback result and actual values without exposing the path.
- DELETE /api/v1/jobs/{jobId} can cancel queued work only. Running Windows file operations are not falsely reported as cancelled.
- On shutdown, queued jobs are marked cancelled and running operations are allowed to finish before worker threads are joined.

File writes and readback remain outside the foobar2000 UI thread. Only the playlist lookup is dispatched through the SDK's main-thread callback queue with a bounded wait.

## Frontend origin and bootstrap dependency

CrayonShell's production origin is https://foo-ui-webview2.local/; Vite development uses http://localhost:5173. The exact allowed origins are configured in the transport. No wildcard CORS is used.

The component's port and bearer token are not exposed by an unauthenticated HTTP endpoint and are never written to logs. After the listener starts, the Bridge atomically publishes `<foobar2000-profile>\\foo_crayonshell_bridge\\service.json` with a protected DACL for the current user and LocalSystem. CrayonShell discovers the profile through the existing `misc.getProfilePath` method and reads the descriptor through the existing `file.read` method, which allows reads inside the profile. The Bridge removes the file before listener shutdown and refreshes its 24-hour expiry every six hours. The host's existing WebView2 message gate rejects messages from untrusted origins. No host source changes or new host API are required.

Standard CORS preflight is implemented for the Authorization and Content-Type headers. Verify the installed WebView2 runtime's Local Network Access (LNA) behavior and loopback permission during actual testing. Do not add the deprecated PNA allow header; current Chromium guidance uses browser-side local/loopback network permissions.

## foobar2000 metadb rules

- Never edit foobar2000 database files or SQLite storage directly.
- Keep filesystem metadata operations separate from metadb refresh outcomes.
- Do not trigger a full tag reload merely because a timestamp was read or written.
- metadbRefresh remains disabled. A timestamp-only write has not yet been shown to require a metadb notification; any future action must use a supported SDK interface and be justified by installed-host testing.

## Remaining delivery phase

### Phase E — CrayonShell integration and hardening

- Use the existing trusted WebView2 APIs `misc.getProfilePath` and `file.read` to obtain the descriptor.
- Handle server restart, token rotation, missing component, timeout and version mismatch through the typed React client.
- Verify production/dev CORS and Local Network Access (LNA) behavior in the embedded WebView2 runtime.
- Test the installed x86 component inside foobar2000 and validate x64 as well.

## Explicit non-goals

- No PluginRegistry dependency for the primary transport.
- No direct metadb/database edits.
- No arbitrary path access from JavaScript.
- No LAN exposure or wildcard socket binding.
- No unbounded thread or connection queue.
- No claim that Windows ChangeTime is writable.
- No synchronous file I/O or playlist enumeration on the foobar2000 UI thread.
