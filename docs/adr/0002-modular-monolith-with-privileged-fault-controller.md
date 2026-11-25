# ADR-0002: Modular monolith with a separate privileged fault controller

- Status: Proposed
- Date: 2026-11-25

## Context
The brief lists ~15 components and asks for separate processes "only when justified". Running each
as a service would add network hops, deployment surface and failure modes without benefit at the
MVP scale (one host, three database nodes).

Fault injection is different: it needs the Docker Engine API (effectively root on the host) and the
Toxiproxy admin API. Giving those to the analysis/API process would make any API bug a host
compromise.

## Decision
- `faulttwind`: one C++ process composed of libraries (`libs/*`) with interface-based boundaries.
- `ft-faultctl`: a separate C++ process, only in the `lab` Compose profile, the sole holder of the
  Docker socket. It enforces environment guards itself (it does not trust `faulttwind`).
- `ft-loadgen`: separate so its latency measurements are not distorted by the core's CPU use.
- Python training is offline only.

## Alternatives considered
- **Microservices per component**: more realistic-looking, but premature; costs build/CI time and
  adds distributed failure modes that are not the subject of the project.
- **Everything in one process including fault injection**: simplest, but violates least privilege.

## Consequences
- Library boundaries must be real: no cross-library includes of private headers, no dependency
  cycles (checked in CI from Phase 2).
- A future split (e.g. simulation workers) is a deployment change, because interfaces already exist.

## Revisit when
Simulation CPU contention measurably degrades ingestion latency (Phase 11), or collectors must run
on separate hosts.
