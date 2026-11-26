# HTTP API v1 contract (initial slice)

This document defines the first browser-facing API surface. It is a contract for implementation, not a claim that the endpoints are already available.

## Transport

- Base URL: `http://127.0.0.1:<ephemeral-port>/api/v1`.
- HTTP/1.1, JSON UTF-8.
- One request per TCP connection in the initial implementation; response includes `Connection: close`.
- All routes require `Authorization: Bearer <instance-token>`.
- Requests must originate from the exact production WebView2 origin or the explicitly enabled Vite development origin.
- The listener rejects non-loopback peers and unexpected Host/Origin values.
- The token is delivered only through the trusted WebView2 host bootstrap. It is never returned by an unauthenticated HTTP route or placed in a URL.

## Common response envelope

Success:

```json
{
  "success": true,
  "data": {}
}
```

Failure:

```json
{
  "success": false,
  "error": {
    "code": "INVALID_PLAYLIST_INDEX",
    "message": "The requested playlist does not exist."
  }
}
```

Error messages are suitable for display and must not disclose filesystem paths, tokens, or internal implementation details. Machine-readable `code` values are stable; messages may evolve.

## Endpoints

### GET /system/health

Returns protocol and component information.

```json
{
  "success": true,
  "data": {
    "protocolVersion": 1,
    "componentVersion": "0.2.0",
    "uptimeMs": 12000,
    "capabilities": {
      "readCreationTime": true,
      "writeCreationTime": false,
      "readLastAccessTime": false,
      "writeLastAccessTime": false,
      "readLastWriteTime": false,
      "writeLastWriteTime": false,
      "readChangeTime": false,
      "writeChangeTime": false,
      "metadbRefresh": false
    }
  }
}
```

Capability flags are derived from implemented and tested behavior, not merely from Windows API declarations.

### GET /playlists/{playlistIndex}/items/{itemIndex}/timestamps

The route accepts non-negative decimal playlist and item indices. It resolves the selected entry through foobar2000's playlist manager and does not accept a filesystem path.

Initial response:

```json
{
  "success": true,
  "data": {
    "playlistIndex": 0,
    "itemIndex": 0,
    "timestamps": {
      "creationTime": {
        "supported": true,
        "readable": true,
        "writable": true,
        "unixMilliseconds": 1795600000000
      },
      "lastAccessTime": {
        "supported": true,
        "readable": true,
        "writable": true,
        "unixMilliseconds": 1795600000000
      },
      "lastWriteTime": {
        "supported": true,
        "readable": true,
        "writable": true,
        "unixMilliseconds": 1795600000000
      },
      "changeTime": {
        "supported": true,
        "readable": true,
        "writable": false,
        "unixMilliseconds": 1795600000000
      }
    }
  }
}
```

The example shows the target schema, not a guarantee that all four fields are supported on every filesystem. A field that cannot be read must be represented with `readable: false` and a stable field-level error. `changeTime` is read-only unless a supported write path is demonstrated and tested.

### PATCH /playlists/{playlistIndex}/items/{itemIndex}/timestamps

Accepts only explicitly supported fields. Unknown fields are rejected; the server must not silently ignore them.

Request example:

```json
{
  "timestamps": {
    "creationTime": 1795600000000,
    "lastWriteTime": 1795600000000
  }
}
```

Initial implementation may return `202 Accepted` with a job ID:

```json
{
  "success": true,
  "data": {
    "jobId": "opaque-random-id",
    "state": "queued"
  }
}
```

A job must resolve and authorize the selected playlist item before modifying the file. The operation uses a filesystem handle with minimum required access, applies only supported timestamp fields, then reads the values back. It must not rely solely on the requested values.

### GET /jobs/{jobId}

Returns one of `queued`, `running`, `completed`, `failed`, or `cancelled`.

Completed example:

```json
{
  "success": true,
  "data": {
    "jobId": "opaque-random-id",
    "state": "completed",
    "result": {
      "filesystem": {
        "success": true,
        "timestamps": {}
      },
      "metadb": {
        "requested": false,
        "state": "not_requested"
      }
    }
  }
}
```

Filesystem and metadb outcomes are separate. A successful timestamp write must not imply that foobar2000's metadata database has refreshed.

### DELETE /jobs/{jobId}

Requests cancellation only while the job is still queued. Once an OS file operation or SDK operation has begun, cancellation may be unavailable and must return a conflict response rather than falsely claiming cancellation.

## Initial error codes

- `UNAUTHORIZED`
- `ORIGIN_NOT_ALLOWED`
- `INVALID_REQUEST`
- `INVALID_JSON`
- `PAYLOAD_TOO_LARGE`
- `ROUTE_NOT_FOUND`
- `METHOD_NOT_ALLOWED`
- `INVALID_PLAYLIST_INDEX`
- `INVALID_ITEM_INDEX`
- `ITEM_NOT_AVAILABLE`
- `FILE_ACCESS_DENIED`
- `FILE_NOT_FOUND`
- `TIMESTAMP_NOT_SUPPORTED`
- `TIMESTAMP_WRITE_FAILED`
- `METADB_REFRESH_FAILED`
- `JOB_NOT_FOUND`
- `JOB_NOT_CANCELLABLE`
- `SERVICE_STOPPING`
- `INTERNAL_ERROR`

## Timestamp semantics

The API uses Windows terminology:

- `creationTime` = Windows file creation time.
- `lastAccessTime` = Windows last-access time.
- `lastWriteTime` = Windows last-write time.
- `changeTime` = Windows change time, not POSIX ctime.

All values are Unix milliseconds on the wire. Internal code preserves FILETIME precision. Missing or unsupported values are not represented as zero or as a fabricated timestamp.

## Metadb consistency

A timestamp write and a foobar2000 metadata refresh are separate operations. The component must use the pinned foobar2000 SDK's supported interfaces and must never edit foobar2000's database files directly. The first implementation must verify whether a timestamp-only change actually requires a metadata reload before enabling the `metadbRefresh` capability.
