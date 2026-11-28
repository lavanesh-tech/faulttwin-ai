# ADR-0003: REST between processes now; Protobuf schemas now; gRPC deferred; Asio only where needed

- Status: Accepted in part. The REST and gRPC-deferral parts hold. The Protobuf incident log is
  replaced by the meta-db in Phase 5 (ADR-0020, ADR-0022).
- Date: 2026-11-25

## Context
The brief allows gRPC/Protobuf and Boost.Asio "where justified". Inter-process traffic in the MVP is
low volume: `faulttwind` → `ft-faultctl` commands (a few per experiment) and `ft-loadgen` histogram
snapshots (one per second). Drogon (on Trantor) already provides an event loop, HTTP server and
HTTP client.

## Decision
- Use authenticated REST/JSON (Drogon) for inter-process calls in the MVP.
- Define Protobuf schemas (`proto/faulttwin/v1`) in Phase 2–5 and use them for the **incident log and
  segment file format**, where schema evolution and compact binary encoding are genuinely valuable.
- Introduce gRPC (with the same messages) when a remote collector agent is built, i.e. a streaming,
  high-volume cross-process telemetry path exists.
- Use Boost.Asio only in components that need raw socket timing (e.g. TCP RTT probes) or timers
  outside Drogon's loop. Do not run two general-purpose event-loop frameworks without a reason.

## Alternatives considered
- gRPC everywhere from day one: adds a heavy dependency (long source builds) for no measured benefit.
- JSON for the incident log: larger, slower, weaker schema evolution guarantees.

## Consequences
- Protobuf is a Phase 3 dependency; gRPC is not in the initial vcpkg manifest.
- The README must say honestly that gRPC is designed-for but deferred, if it remains so.

## Revisit when
A collector agent process is introduced, or REST overhead appears in a benchmark.
