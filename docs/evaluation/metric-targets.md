# Metric Targets (planning only)

These are the owner's provisional stretch goals from the master specification §10. **They are
targets, not results.** None has been measured. They will be revised after initial measurements,
hardware limits and experimental validity are understood. An unmet target is reported as unmet.

| Metric | Target | Metric IDs | Status |
|---|---|---|---|
| Synthetic telemetry workload | ≥ 1,000,000 events across benchmark runs | ING-ATT | PLANNED |
| Sustained ingestion | ≥ 10,000 events/s | ING-TPUT | PLANNED |
| Ingestion p99 latency | < 20 ms at a documented workload | ING-LAT-p99 | PLANNED |
| Overload testing | multiple sustained and burst profiles | Exp. 2 | PLANNED |
| Throughput improvement | ≥ 2× over single-threaded baseline | REL-THR | PLANNED |
| Peak memory reduction | ≥ 30% vs reference design | REL-MEM | PLANNED |
| ML prediction quality | F1 ≥ 0.85 on held-out scenarios | DET-F1 | PLANNED |
| False-alarm reduction | ≥ 20% vs tuned rules | REL-FA | PLANNED |
| Prediction lead time | 15–60 s for suitable developing failures | DET-LEAD | PLANNED |
| Recovery downtime reduction | ≥ 20% vs valid baseline | REL-DT | PLANNED |
| Twin downtime error | evaluate whether MAPE < 15% is achievable | SIM-DT-MAPE | PLANNED |
| Fault scenario configurations | ≥ 30 distinct | Exp. 4–5 | PLANNED |
| Core module line coverage | ≥ 85%, critical branches reviewed | — | PLANNED (coverage not yet measured) |
| Deterministic replay | identical required outputs | RPL-SUCC, SIM-DET | PLANNED |
