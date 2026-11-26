# Metric Targets (planning only)

These are the owner's provisional stretch goals from the master specification §10. **They are
targets, not results.** None has been measured. They will be revised after initial measurements,
hardware limits and experimental validity are understood. An unmet target is reported as unmet.

| Metric | Target | Metric IDs | Status |
|---|---|---|---|
| Synthetic telemetry workload | ≥ 1,000,000 events across benchmark runs | ING-ATT | MEASURED (Phase 2, container): 1,000,000 per configuration |
| Sustained ingestion | ≥ 10,000 events/s | ING-TPUT | MEASURED (Phase 2, container): met in every configuration (lowest 165k events/s) |
| Ingestion p99 latency | < 20 ms at a documented workload | ING-LAT-p99 | MEASURED (Phase 2, container): met at capacity 1024; not met at capacity 16384 with heavy work (up to 103 ms) |
| Overload testing | multiple sustained and burst profiles | Exp. 2 | PARTIAL: one sustained profile only (Phase 2) |
| Throughput improvement | ≥ 2× over single-threaded baseline | REL-THR | MEASURED (Phase 2, container): met with heavy work (up to 3.5×); not met with light work (≤ 0.42×) |
| Peak memory reduction | ≥ 30% vs reference design | REL-MEM | MEASURED (reduced Exp. 2, container): not met by RSS (−8.8%); queue depth −99.5% |
| ML prediction quality | F1 ≥ 0.85 on held-out scenarios | DET-F1 | PLANNED |
| False-alarm reduction | ≥ 20% vs tuned rules | REL-FA | PLANNED |
| Prediction lead time | 15–60 s for suitable developing failures | DET-LEAD | PLANNED |
| Recovery downtime reduction | ≥ 20% vs valid baseline | REL-DT | PLANNED |
| Twin downtime error | evaluate whether MAPE < 15% is achievable | SIM-DT-MAPE | PLANNED |
| Fault scenario configurations | ≥ 30 distinct | Exp. 4–5 | PLANNED |
| Core module line coverage | ≥ 85%, critical branches reviewed | — | PLANNED (coverage not yet measured) |
| Deterministic replay | identical required outputs | RPL-SUCC, SIM-DET | PLANNED |
