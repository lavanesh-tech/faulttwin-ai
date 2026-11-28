# ADR-0019: In-tree Prometheus metrics and JSON-lines logging

- Status: Accepted (Phase 5)
- Date: 2026-11-28

## Context
Phase 5 needs self-observability for `faulttwind`: Prometheus metrics and structured logs.
The dependency plan listed spdlog for logging and left the metrics library open. The needs
are small:
- about a dozen metric families;
- one process, scraped every 5 s;
- logs that a machine can parse and that never leak a secret.

Each third-party library adds a pinned dependency, a licence to track and a build-time cost.

## Decision
1. **Metrics are in-tree** (`include/faulttwin/observability/metrics.hpp`).
   - Types: `Counter`, `Gauge` and `Histogram`. The hot path is lock-free (atomics, with a
     CAS loop for `double`).
   - A `Registry` renders Prometheus text format 0.0.4 in a deterministic order, so the
     output can be tested byte for byte.
   - Every family has a limit on its number of label combinations (default 256). A sample
     past the limit goes to a shared overflow series, and
     `faulttwin_metrics_series_overflow_total` counts these. A label taken from untrusted
     input therefore cannot grow memory without bound.
   - Metric and label names are validated, and label values are escaped.
2. **Logs are JSON lines on stderr** (`include/faulttwin/core/logger.hpp`).
   - Each record has `ts`, `level`, `component` and `msg`, followed by typed fields.
   - A field whose key looks like a secret (password, token, api_key, authorization, …) is
     always written as `"[redacted]"`.
   - Free text (`msg` and string fields) also has `Bearer …` and `key=value` secrets
     redacted, because libpq error messages can echo connection parameters.
   - A field that reuses an envelope name is written as `field.<name>`, so a field can never
     overwrite the envelope.
3. **Metric names in Phase 5:**

   | Area | Metrics |
   |---|---|
   | HTTP | `faulttwin_http_requests_total{route,method,status}`, `faulttwin_http_request_duration_seconds{route}` |
   | Monitor | `faulttwin_monitor_cycles_total`, `faulttwin_monitor_cycle_errors_total`, `faulttwin_monitor_cycle_duration_seconds`, `faulttwin_detections_total{kind,severity}`, `faulttwin_node_up{node}` |
   | Recorder | `faulttwin_incident_operations_total{op}`, `faulttwin_incident_detections_dropped_total`, `faulttwin_incident_detections_rejected_total`, `faulttwin_incident_store_retries_total`, `faulttwin_incident_recorder_pending` |

   The `route` label is the route template (for example `/api/v1/incidents/{id}`), never the
   raw path.

## Alternatives considered
- **prometheus-cpp:** a full client with a built-in HTTP exposer. That exposer is a second
  HTTP server beside Drogon, and the library adds a dependency for about 300 lines of code.
- **spdlog:** fast and well known, but its JSON output needs a custom formatter, and
  redaction would still be our own code. It also adds fmt.
- **OpenTelemetry C++:** the right choice once traces cross processes. It is too heavy for
  one process in Phase 5.

## Consequences
- Two more small components are covered by unit tests: rendering, escaping, the overflow
  counter and redaction.
- Exemplars, summaries and the OpenMetrics format are not supported.
- The logger serialises writes with one mutex. That is fine at Phase 5 log volume, and it is
  measured again in Phase 11.

## Revisit when
Traces across processes are needed (Phase 8 onward), or the logger shows up in a profile.
