# asterion_research

Research owns its durable research tasks (backtest, factor, statistics, samples, models, evaluations and reviews): admission, attempts, progress,
cancellation, explicit retry, result confirmation and restart recovery. Data owns
minute acquisition, table updates, publication and its own durable task ledger.
There is no independent Task service or generic task submission endpoint.

Research binds an explicit Data instance through its public service endpoint.
Multiple Research instances may bind the same Data service. Each has its own
ledger and bounded calculation pool; Agent only supervises service lifecycle.
Shared task records and persistence mechanics live in `apps/services/task-runtime`,
not Core. Terminal aggregates tasks with their node, service and connection owner.

The I/O owner serializes state transitions. Computation receives immutable input,
cooperative cancellation and a progress channel. Restart interrupts unconfirmed
work; retry remains explicit. Upgrade drains the owning service first.

Startup reads `--config <node>/config/services/<instance>/research.json`.
`task_concurrency` defaults to 1 and permits 1–2. Deployment copies defaults once;
ordinary startup does not generate or convert configuration or existing data.
See [architecture](../../../docs/architecture.md).
