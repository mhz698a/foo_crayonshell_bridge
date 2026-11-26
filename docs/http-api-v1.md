# HTTP API v1

This contract describes the implemented local API. All routes are served from the loopback base URL http://127.0.0.1:<ephemeral-port>/api/v1.

## Transport and security

- HTTP/1.1, UTF-8 JSON, one request per TCP connection.
- Connection close; no keep-alive, upgrades or CONNECT.
- Every route requires Authorization: Bearer <instance-token>.
- Exact production WebView2 origin or explicitly enabled Vite development origin only.
- The listener binds exclusively to 127.0.0.1.
- No arbitrary filesystem paths, token query parameters or unauthenticated service descriptor endpoint.

## Common response envelope

Success:

    {
      "success": true,
      "data": {}
    }

Failure:

    {
      "success": false,
      "error": {
        "code": "INVALID_PLAYLIST_INDEX",
        "message": "The requested playlist does not exist."
      }
    }

Messages do not disclose filesystem paths, tokens or internal details. Error codes are stable machine-readable identifiers. Filesystem-operation errors also include the numeric `win32Error` returned by the Windows API, allowing the caller to distinguish the underlying OS failure without exposing the file path.

## GET /system/health

Returns protocol/component version, service uptime and implemented capability flags.

    {
      "success": true,
      "data": {
        "protocolVersion": 1,
        "componentVersion": "0.1.1",
        "uptimeMs": 12000,
        "capabilities": {
          "readFileSize": true,
          "readCreationTime": true,
          "writeCreationTime": true,
          "readLastAccessTime": true,
          "writeLastAccessTime": true,
          "readLastWriteTime": true,
          "writeLastWriteTime": true,
          "readChangeTime": true,
          "writeChangeTime": false,
          "metadbRefresh": false
        }
      }
    }

The write flags describe the API's implemented capability. An individual filesystem write can still fail because of permissions, sharing mode, media removal or filesystem limitations.

## GET /playlists/{playlistIndex}/items/{itemIndex}/timestamps

Accepts non-negative decimal playlist and item indices. The route resolves the row through foobar2000's playlist manager and never accepts a caller-supplied path.

The response returns the file size in bytes as `fileSizeBytes`, plus creationTime, lastAccessTime, lastWriteTime and Windows changeTime as Unix milliseconds. The preferred read uses `GetFileInformationByHandleEx(FileBasicInfo)` and `FileStandardInfo`; where those structures are unavailable, the component falls back to `GetFileInformationByHandle` and preserves the file size and the three standard timestamps. In fallback mode ChangeTime is unavailable rather than being substituted with another date. File size is never derived from Title Formatting. The first three fields are writable. changeTime is read-only.

The HTTP worker asks foobar2000's main-thread callback queue to resolve playlist coordinates. That callback performs only SDK work and copies the physical path. Windows file-attribute I/O runs outside the UI thread. The physical path is never returned.

## PATCH /playlists/{playlistIndex}/items/{itemIndex}/timestamps

Accepts Content-Type: application/json. The body must be a JSON object containing one or more of the following fields:

    {
      "creationTime": 1700000000000,
      "lastAccessTime": 1700000001000,
      "lastWriteTime": 1700000002000
    }

Only fields present in the request are changed. Values are unsigned Unix milliseconds and must fit the signed Windows FILETIME range. Duplicate keys, unknown fields, strings, negative values, decimals, exponents, nulls and out-of-range timestamps are rejected. A changeTime field is explicitly rejected with 422 TIMESTAMP_FIELD_NOT_WRITABLE.

The route first resolves the playlist row through the SDK main-thread queue and captures its physical path internally. The write is then queued for a dedicated filesystem worker. The response is 202 Accepted:

    {
      "success": true,
      "data": {
        "jobId": "0123456789abcdef0123456789abcdef",
        "status": "queued",
        "playlistIndex": 0,
        "itemIndex": 0,
        "statusUrl": "/api/v1/jobs/0123456789abcdef0123456789abcdef"
      }
    }

The opaque job identifier is generated using Windows CNG. The response and job status never expose the physical path.

## GET /jobs/{jobId}

Returns queued, running, succeeded, completed_unverified, failed or cancelled.

A completed job reports whether SetFileTime succeeded, whether readback succeeded, the Windows error codes, and the actual timestamps read back from the file. A successful write with failed readback is reported as completed_unverified, not as a verified success.

The job manager has two workers, a queue capacity of sixteen waiting jobs and a bounded history of 128 records. Older terminal records may be evicted when the history reaches its limit.

## DELETE /jobs/{jobId}

Cancellation succeeds only while a job is still queued. Once a worker has changed the state to running, the filesystem operation is not cancellable. Running jobs return 409 JOB_ALREADY_RUNNING; terminal jobs return 409 JOB_ALREADY_COMPLETED.

When the component shuts down, queued jobs are marked cancelled. A filesystem operation that has already started is allowed to finish before its owned worker thread is joined.

## Timestamp terminology

- creationTime: Windows creation time.
- lastAccessTime: Windows last-access time.
- lastWriteTime: Windows last-write time (mtime).
- changeTime: Windows ChangeTime, corresponding to NTFS metadata-change time; it is not creation time and is read-only.
- fileSizeBytes: current stream length, as a non-negative integer byte count.

Values on the wire are Unix milliseconds. FILETIME precision is retained internally until serialization. Missing or out-of-range values are never fabricated as zero.

## metadb consistency

A filesystem timestamp write and a foobar2000 metadb update are independent outcomes. The component never edits foobar2000 database files. metadbRefresh remains false and each job reports metadbRefresh.status as not_attempted.

A timestamp-only write has not yet been demonstrated to require a metadb notification in the installed host. No unsupported notification or full tag reload is issued speculatively. This capability will remain disabled until runtime testing establishes a need and a supported SDK mechanism.
