# ADR-0009: The LLM explains; it never acts

- Status: Proposed
- Date: 2026-11-25

## Context
LLMs can hallucinate and can be prompt-injected via data they read (e.g. query text in telemetry).
Letting one choose or execute recovery actions on databases is unsafe.

## Decision
- The explanation service receives a structured evidence bundle and returns structured JSON.
- It has no tools/function calling and its only dependency is an HTTPS transport.
- A deterministic validator checks evidence IDs and numeric claims; unverifiable claims are dropped
  and the explanation is labelled accordingly.
- Untrusted strings in evidence (query text, labels) are truncated and quoted as data.
- Budget, caching, circuit breaker and a template fallback make the feature optional.

## Consequences
- Explanations may be less fluent when evidence is thin; that is the intended behavior.
