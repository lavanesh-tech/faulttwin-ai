# ADR-0005: Custom bounded time-series store; Prometheus for self-observability only

- Status: Proposed
- Date: 2026-11-25

## Context
Features, replay and the UI need recent per-series data with range/step queries. Off-the-shelf
TSDBs (Prometheus, TimescaleDB) would work, but the project's purpose is native systems engineering,
and the required query surface is small.

## Decision
Implement `tsdb`: sharded in-memory fixed-capacity ring buffers (hot) plus append-only, CRC-checked
Protobuf segment files (warm). Prometheus scrapes FaultTwin's *own* metrics.

## Alternatives considered
- Prometheus as the telemetry store: pull model and 15 s typical scrape do not fit sub-second
  ingestion and replay; query API not designed for embedding.
- TimescaleDB: excellent, but puts a database on the hot path of the system that monitors databases.

## Consequences
- We own correctness: corruption detection, retention, memory bounds must be tested.
- Not a general-purpose TSDB; documented as such.
