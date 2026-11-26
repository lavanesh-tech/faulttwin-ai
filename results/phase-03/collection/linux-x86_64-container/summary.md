# Collection measurements (linux-x86_64-container)

## A. Idle cluster, 300 cycles at 200 ms, 3 nodes

- Collection success (MON-SUCC): 100.00 % (900/900)
- Coverage (MON-COV): 100.00 % (16503/16503 applicable readings ok)
- First cycle (connects to 3 nodes): wall 49.21 ms, client CPU 15.55 ms

| Metric (cycles 2-300) | mean | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| cycle wall ms (3 nodes) | 15.16 | 14.65 | 20.51 | 28.65 | 51.08 |
| per-node collect wall ms | 5.01 | 4.52 | 8.05 | 10.46 | 44.13 |
| client CPU ms per cycle | 1.73 | 1.69 | 2.26 | 2.57 | 2.95 |
| freshness at report ms (MON-FRESH) | 4.27 | 3.83 | 10.23 | 13.47 | 31.91 |

Server-side execution time of the monitor's own statements (pg_stat_statements):

| node | statements | total exec ms | exec ms per cycle |
|---|---|---|---|
| pg-primary | 3600 | 107.720 | 0.359 |
| pg-replica-1 | 3300 | 70.042 | 0.233 |
| pg-replica-2 | 3300 | 62.765 | 0.209 |

## B. pgbench throughput (4 clients, 15 s, single-row inserts)

| condition | runs | tps per run | mean tps | vs none |
|---|---|---|---|---|
| none | 3 | 5555, 5438, 5344 | 5446 | — |
| monitor-1000ms | 3 | 5462, 5450, 5261 | 5391 | -1.0 % |
| monitor-100ms | 3 | 5412, 5136, 5200 | 5249 | -3.6 % |

Run-to-run spread (max - min) / mean: none 3.9 %, monitor-1000ms 3.7 %, monitor-100ms 5.3 %. Differences smaller than the spread are not distinguishable from noise.
