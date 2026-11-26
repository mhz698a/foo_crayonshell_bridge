# HTTP API v1

This contract describes the implemented read-only slice and the planned write/job surface. An endpoint is not considered available until its implementation and tests are present.

## Transport

- Base URL: `http://127.0.0.1:<ephemeral-port>/api/v1`.
- HTTP/1.1, JSON UTF-8, one request per TCP connection.
- `Connection: close`, no keep-alive, no upgrades, no CONNECT.
- Every route requires `Authorization: Bearer <instance-token>`.
- Exact production WebView2 origin or explicitly enabled Vite development origin only.
- No arbitrary filesystem paths, token query parameters or unauthenticated descriptor endpoint.

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

Messages do not disclose filesystem paths, tokens or internal details. Error codes are stable machine-readable identifiers.

## Implemented read-only endpoints

### GET /system/health

Returns protocol and component version, service uptime and capability flags.

```json
{
  "success": true,
  "data": {
    "protocolVersion": 1,
    "componentVersion": "0.1.0",
    "uptimeMs": 12000,
    "capabilities": {
      "readCreationTime": true,
      "writeCreationTime": false,
      "readLastAccessTime": true,
      "writeLastAccessTime": false,
      "readLastWriteTime": true,
      "writeLastWriteTime": false,
      "readChangeTime": true,
      "writeChangeTime": false,
      "metadbRefresh": false
    }
  }
}
```

Capability flags describe implemented routes. Filesystem-specific read failures are reported per item/field.

### GET /playlists/{playlistIndex}/items/{itemIndex}/timestamps

Accepts non-negative decimal playlist and item indices. The route resolves the row through foobar2000's playlist manager and never accepts a caller-supplied path.

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
        "writable": false,
        "unixMilliseconds": 1795600000000
      },
      "lastAccessTime": {
        "supported": true,
        "readable": true,
        "writable": false,
        "unixMilliseconds": 1795600000000
      },
      "lastWriteTime": {
        "supported": true,
        "readable": true,
        "writable": false,
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

The example illustrates the schema. Real values come from `GetFileInformationByHandleEx(FileBasicInfo)`; a field with no representable value is returned with `readable: false` and `TIMESTAMP_UNAVAILABLE`. The physical path is never returned.

Playlist lookup runs on foobar2000's main thread through the SDK callback queue. The callback only resolves the selected `metadb_handle` and copies its physical path. Windows file attribute I/O runs on an HTTP worker, not the UI thread. A timed-out SDK dispatch returns `503 SDK_DISPATCH_TIMEOUT`.

### Error codes for read endpoints

- `UNAUTHORIZED`
- `ORIGIN_NOT_ALLOWED`
- `INVALID_HOST`
- `INVALID_PLAYLIST_INDEX`
- `INVALID_ITEM_INDEX`
- `INVALID_PLAYLIST_COORDINATES`
- `SDK_DISPATCH_TIMEOUT`
- `SDK_THREAD_VIOLATION`
- `FILE_ACCESS_DENIED`
- `FILE_NOT_FOUND`
- `ITEM_NOT_SUPPORTED`
- `FILE_METADATA_READ_FAILED`
- `ROUTE_NOT_FOUND`
- `METHOD_NOT_ALLOWED`
- `SERVICE_STOPPING`
- `INTERNAL_ERROR`

## Planned, not yet implemented

### PATCH /playlists/{playlistIndex}/items/{itemIndex}/timestamps

The write route will accept only explicitly supported timestamp fields and will return a bounded job identifier when execution is asynchronous. It is not implemented in the current read-only phase.

### GET /jobs/{jobId} and DELETE /jobs/{jobId}

The job manager is not implemented yet. Cancellation will only be accepted while a job remains queued; an OS or SDK operation that has begun will not be reported as cancelled.

## Timestamp terminology

- `creationTime`: Windows creation time.
- `lastAccessTime`: Windows last-access time.
- `lastWriteTime`: Windows last-write time.
- `changeTime`: Windows ChangeTime, not POSIX ctime; read-only.

Values are Unix milliseconds on the wire. FILETIME precision is retained internally until serialization. Missing or out-of-range values are not fabricated as zero.

## metadb consistency

A filesystem timestamp operation and a foobar2000 metadb refresh are independent outcomes. The component never edits foobar2000's database files. The first release leaves `metadbRefresh: false`; Phase D will verify whether a timestamp-only change requires any supported metadb notification before enabling it.
