# ADR-0008: Select the failure-prediction model empirically; export to ONNX

- Status: Proposed (decided in Phase 7)
- Date: 2026-11-25

## Context
Inputs are windowed tabular features from lab-generated scenarios; datasets are small; inference
must run natively in C++ through ONNX Runtime; the model must be explainable and compared fairly
against a tuned rule baseline.

## Decision
Train logistic regression, random forest and gradient boosting on identical leakage-safe splits
(split by independent scenario run; preprocessing fitted on training data only; rules and
hyperparameters tuned on validation data). Select on validation metrics, inference cost,
maintainability and ONNX convertibility with the pinned `skl2onnx`. Report results on the held-out
test scenarios once, including negative or inconclusive outcomes.

## Alternatives considered
- Deep sequence models: more data-hungry, harder to calibrate and explain, heavier runtime.
- Unsupervised anomaly scores only: we have ground-truth labels from controlled faults.

## Consequences
- The ML result may be "no better than rules"; that is an acceptable, reportable outcome.
- The feature contract is versioned and shared between Python and C++, with parity tests.
