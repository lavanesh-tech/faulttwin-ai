# faulttwin-detect evaluate

Synthetic scenarios only (illustrative values, not database measurements).
Validation seeds 1-40 (120 episodes) were used for tuning; each test split was evaluated once.
Alert matching: onset <= alert <= end + 30 s, same node.

## Chosen parameters (validation)

| Rule | Tuned trigger | Tuned sustain | Naive trigger |
|---|---|---|---|
| replication_lag_high | 5 | 10 s | 15 |
| replication_lag_growing | 0.1 | 15 s | 0.25 |
| cpu_saturation | 80 | 15 s | 95 |
| statement_latency_with_connection_pressure | 30 | 0 s | 30 |
| telemetry_stale | 1.5 | 2 s | 3.5 |

Learned detectors: trained on seeds 1-20, alert threshold and sustain chosen on seeds 21-40.

| Detector | Trigger (normal-score quantile) | Clear | Sustain |
|---|---|---|---|
| knn_distance | 7.519 (q 0.995) | 3.794 | 10 s |
| perceptron | 2.356 (q 0.9995) | -2.757 | 2 s |

## Split `test` (seeds 1001-1040, intensity 0.8-1.2)

| Detector | Alerts | False alerts | Detected | Precision | Recall | F1 | False alerts/h | Latency p50 | Latency p95 |
|---|---|---|---|---|---|---|---|---|---|
| tuned rules | 144 | 0 | 119/120 | 100.0 % | 99.2 % | 0.996 | 0.00 | 3 s | 33 s |
| naive threshold | 443 | 51 | 110/120 | 88.5 % | 91.7 % | 0.900 | 1.86 | 1 s | 30 s |
| k-NN distance | 114 | 6 | 108/120 | 94.7 % | 90.0 % | 0.923 | 0.22 | 16 s | 37 s |
| averaged perceptron | 57 | 8 | 49/120 | 86.0 % | 40.8 % | 0.554 | 0.29 | 11 s | 42 s |

Detected per fault class (latency p50):

| Detector | replication_lag_buildup | cpu_saturation | overload | node_down |
|---|---|---|---|---|
| tuned rules | 25/26 (21 s) | 34/34 (15 s) | 35/35 (0 s) | 25/25 (3 s) |
| naive threshold | 16/26 (24 s) | 34/34 (0 s) | 35/35 (0 s) | 25/25 (3 s) |
| k-NN distance | 19/26 (27 s) | 31/34 (33 s) | 34/35 (10 s) | 24/25 (14 s) |
| averaged perceptron | 25/26 (19 s) | 0/34 (N/A) | 0/35 (N/A) | 24/25 (4 s) |

## Split `test_shifted` (seeds 2001-2040, intensity 0.5-0.8)

| Detector | Alerts | False alerts | Detected | Precision | Recall | F1 | False alerts/h | Latency p50 | Latency p95 |
|---|---|---|---|---|---|---|---|---|---|
| tuned rules | 142 | 0 | 113/120 | 100.0 % | 94.2 % | 0.970 | 0.00 | 3 s | 30 s |
| naive threshold | 252 | 40 | 81/120 | 84.1 % | 67.5 % | 0.749 | 1.45 | 3 s | 39 s |
| k-NN distance | 108 | 5 | 103/120 | 95.4 % | 85.8 % | 0.904 | 0.18 | 22 s | 39 s |
| averaged perceptron | 68 | 8 | 60/120 | 88.2 % | 50.0 % | 0.638 | 0.29 | 5 s | 31 s |

Detected per fault class (latency p50):

| Detector | replication_lag_buildup | cpu_saturation | overload | node_down |
|---|---|---|---|---|
| tuned rules | 29/33 (21 s) | 25/28 (15 s) | 27/27 (0 s) | 32/32 (3 s) |
| naive threshold | 21/33 (28 s) | 1/28 (1 s) | 27/27 (0 s) | 32/32 (3 s) |
| k-NN distance | 24/33 (30 s) | 26/28 (35 s) | 27/27 (10 s) | 26/32 (14 s) |
| averaged perceptron | 29/33 (20 s) | 0/28 (N/A) | 0/27 (N/A) | 31/32 (4 s) |
