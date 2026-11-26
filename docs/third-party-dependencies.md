# Third-party dependencies

## llhttp

- Upstream: https://github.com/nodejs/llhttp
- Release: `release/v9.4.4`
- Release commit prefix: `8164852`
- Release date: 2026-11-13
- License: MIT
- License file staged by CI: `vendor/llhttp/LICENSE`
- Sources staged by CI:
  - `include/llhttp.h`
  - `src/llhttp.c`
  - `src/api.c`
  - `src/http.c`

The GitHub Actions workflow checks out the release tag, verifies the checked-out commit prefix and the public header's 9.4.4 version macros, then stages these files under the ignored `vendor/llhttp/` directory. The C sources are compiled directly by MSVC for both Win32 and x64. Node.js/npm/llparse are not build-time requirements for this component.

The MIT license notice is retained in the distributed source repository and is also copied into the generated `.fb2k-component` package as `licenses/llhttp-MIT.txt`, so the binary package carries the notice with it.

The parser is used in strict mode. The component must not enable llhttp's lenient parsing options without a separate security review.
