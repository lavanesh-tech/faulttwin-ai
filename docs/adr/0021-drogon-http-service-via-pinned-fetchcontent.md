# ADR-0021: Drogon HTTP service, pinned through FetchContent, behind a transport-free API layer

- Status: Accepted (Phase 5). Settles the Phase 5 part of ADR-0004.
- Date: 2026-11-28

## Context
The master spec names Drogon for the HTTP service. ADR-0004 said that Phase 5 would decide
between FetchContent and vcpkg "with measured build times". The service must:
- be testable without sockets;
- shut down cleanly on SIGTERM;
- keep third-party warnings out of the `-Werror` build;
- build on Ubuntu 24.04 CI and on the owner's macOS machine.

## Decision
1. **Drogon v1.9.9 via FetchContent** (`cmake/Drogon.cmake`):
   - pinned to commit `38dd5fea31a7a2727c0a6f6b6b04252374796cab`, the v1.9.9 tag. Its
     Trantor submodule resolves to `8bf280ba043eb77c3eb02121065e0891a9a4f431`;
   - fetched with `SYSTEM`, so Drogon's headers do not trigger our warnings;
   - only the HTTP core is built: no ORM, `drogon_ctl`, examples, Brotli or YAML config;
     static libraries only.

   System libraries required:
   - jsoncpp (Ubuntu `libjsoncpp-dev`; macOS `brew install jsoncpp`);
   - uuid (Ubuntu `uuid-dev`; part of macOS);
   - zlib;
   - OpenSSL is **not** used. Trantor is configured with `TRANTOR_USE_TLS=none`, because with the
     default (use OpenSSL if found) the build depends on whatever OpenSSL the machine has, and
     Trantor v1.5.x does not compile against OpenSSL 4.x (Homebrew, found on the owner's Mac).
     `faulttwind` serves plain HTTP on loopback; TLS is Phase 11 hardening.

   `FAULTTWIN_WITH_DROGON=AUTO|ON|OFF` sets behaviour when jsoncpp is missing. With AUTO,
   `faulttwind` is skipped with a message and everything else still builds and tests.
2. **vcpkg is not adopted.** Drogon is the only heavy dependency, and its measured build cost
   is small (see the phase-05 report, §Build cost). One package manager bootstrap for one
   library is not justified. ADR-0004's FetchContent-with-commit-pin rule continues.
3. **Transport-free API layer.**
   - `ApiService::handle(HttpRequest) -> HttpResponse` holds every rule: routing,
     validation, auth, error format, request ids, metrics and access logs.
   - `src/server/drogon_adapter.cpp` only converts Drogon requests and responses to and from
     those structs.
   - Drogon's router is not used: `setDefaultHandler` sends every request to `handle()`.
   - Unit tests therefore cover the whole API without opening a socket, and an e2e script
     (`scripts/e2e-faulttwind.sh`) covers the real server.
4. **Versioning.**
   - All resources are under `/api/v1`. An unknown version answers 404
     `unsupported_api_version`.
   - Within v1, changes are additive only: new fields or new optional parameters. Removing
     or renaming anything means `/api/v2`.
   - `docs/api/openapi.yaml` is the contract.
5. **Safe defaults.**
   - Listen on `127.0.0.1:8080`. A non-loopback address needs `--allow-remote`, because
     read endpoints have no authentication in Phase 5.
   - The one state-changing endpoint (`acknowledge`) needs `Authorization: Bearer
     $FT_API_KEY`, compared in constant time, with a key of at least 16 characters. With no
     key configured it answers 403 rather than being open.
   - Every response has `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`.
   - Bodies are limited to 16 KiB at the API and 64 KiB in Drogon.
   - The `Server` header is off.
6. **Shutdown.** SIGTERM or SIGINT puts `/readyz` into 503 "draining" and stops Drogon's
   loop. The monitor thread is then joined, and the recorder flushes pending writes before
   exit 0.

## Alternatives considered
- **vcpkg manifest mode:** reproducible binary caching, but it adds a bootstrap, a toolchain
  file and a baseline to maintain, for one library.
- **System Drogon package:** Ubuntu 24.04 has no current Drogon package, and Homebrew's
  version moves.
- **Boost.Beast or cpp-httplib:** lighter, but the spec chose Drogon, and later phases use
  its HTTP client and WebSocket support.
- **Drogon controllers and router:** tie business logic to the framework and need a server
  in every test.

## Consequences
- A first configure downloads Drogon and Trantor from github.com. Later configures reuse
  `build/<preset>/_deps`.
- Each preset builds Drogon once (sanitizer presets build it instrumented).
- Drogon's own deprecation warnings appear in build logs. They are not ours, and `-Werror`
  is not applied to Drogon targets.

## Revisit when
A second heavy dependency arrives (ONNX Runtime, Phase 7), or CI build time grows by more
than a few minutes.
