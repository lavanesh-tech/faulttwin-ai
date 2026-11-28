# ADR-0022: gRPC remains deferred in Phase 5

- Status: Accepted (Phase 5). Updates ADR-0003.
- Date: 2026-11-28

## Context
The Phase 5 scope includes "remote gRPC telemetry transport where justified". ADR-0003 sets
the condition: gRPC arrives when a streaming, high-volume telemetry path crosses process
boundaries, such as a remote collector agent.

In Phase 5, telemetry never leaves the process:
- `faulttwind` runs the Phase 3 collectors in-process and polls PostgreSQL over libpq;
- detection and incident recording happen in the same process;
- the only cross-process traffic is HTTP from operators and Prometheus scrapes.

## Decision
1. Do not add gRPC or Protobuf in Phase 5. No path exists for gRPC to carry, and adding it
   would bring the heaviest build dependency in the project for no measured benefit.
2. The ADR-0003 plan for a Protobuf "incident log" format is replaced by the meta-db
   (ADR-0020). Incidents are queried by state, node and time and are updated by
   acknowledgements, which a database does better than an append-only file.
3. The condition for gRPC stays the same: a remote collector agent, or a measured REST
   overhead. When it is met, telemetry messages will be defined in `proto/faulttwin/v1` and
   served with gRPC beside Drogon.

## Alternatives considered
- **Add a gRPC endpoint that mirrors the REST API:** two transports for the same low-volume
  calls, and one of them unused.
- **Build a remote collector agent now so that gRPC is justified:** this is scope creep. The
  spec does not ask for an agent in Phase 5.

## Consequences
- The README says plainly that gRPC is designed for but not built.
- Phase 6 (fault controller) keeps REST, per ADR-0003.

## Revisit when
A remote collector agent is planned, or a benchmark shows REST/JSON overhead on a hot path.
