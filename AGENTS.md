# Repository Instructions

Rules for anyone changing this repository. Keep this file short and current.

## Scope

- One product: the futures **Terminal** on macOS (Apple Silicon) and Linux x86_64.
  Remote services run on Linux x86_64 only and exist to serve Terminal.
- No Web, mobile, notebook, standalone CLI or Windows clients.
- The project is unreleased. Keep one current implementation: no legacy
  interfaces, aliases, migration paths, version counters or backup copies of code.
  Git holds history.

## Architecture

- Core layers, lowest first: `asterion_foundation` → `asterion_kernel` →
  `asterion_domain`. Dependencies point down. Providers, storage, strategies and
  risk algorithms live in `plugins/` (trusted in-process code, not a sandbox).
- Business services (`apps/services/`): Market, Data, Research and Trading
  (one process per account). Each owns its state and runs its own tasks
  in bounded thread pools. There is no central task service or worker process.
- Management (`apps/management/`): Node Manager (on the Terminal machine) owns
  node registration and Agent connections; Node Agent (on every node) supervises
  that node's service processes. Both are supervised by the OS service manager.
  Neither proxies business traffic: Terminal connects to services directly.
- IPC: Protobuf from `protocol/proto/`; Unix sockets locally, TCP + mTLS remotely.
- Terminal: Electron + React UI (`apps/clients/terminal/src`, `plugins/`), C++
  orchestration in `apps/clients/terminal/native/`. The installer is in
  `apps/clients/installer/`.
- Configuration: defaults in repository `config/`, copied once at deployment to
  `<node>/config/`. Services only read configuration; missing or invalid
  configuration fails with its path.

Details: [docs/architecture.md](docs/architecture.md).

## Trading and data safety

- Market and historical data come from real providers. Never fabricate market
  data; fixtures are for tests only.
- Live trading requires authorization, account risk controls and the single
  execution path. Missing risk configuration or plugins rejects execution.
- Never resend a trading command automatically after a disconnect or an unknown
  outcome.
- Credentials are entered locally and never stored in source, logs, chat,
  configuration files or UI storage. Saved market-data credentials live in the
  OS keychain and are never reused for trading.
- Never silently rewrite, migrate or delete user ledgers, tasks or datasets.

## Engineering

- C++20 with RAII; `Decimal` for money and prices; exceptions never cross C ABI
  or Node-API boundaries. JSON only at protocol and UI boundaries.
- Durable files go through `kernel/durable_file.hpp`.
- Hash only where a digest is an identity (fixed inputs, artifacts, locked
  downloads) or at a trust boundary (cold backup, remote artifacts). Do not
  re-hash or re-validate data the same owner already wrote.
- CMake targets live in their module's `CMakeLists.txt`; names use underscores.
  Dependencies are locked with Conan and pnpm.
- Format with `pnpm run format` (clang-format 23.1.1 and Prettier).
- UI text goes through `src/i18n`; English and Chinese change together.

## Design principles

- Prefer the smallest correct design. Delete code before adding code.
- Fix the root cause; enforce an invariant once, at its owning boundary.
- Fail explicitly on invalid input and broken invariants; add retries or
  fallbacks only for a concrete, reachable failure that is safe to retry.
- One owner per piece of state. No second registry, cache or source of truth.
- Test behavior that matters at the lowest layer that proves it. Do not test
  defensive code that should not exist.

## Workflow

- Work on `main`; do not push unless asked.
- Build and test:
  - C++: `cmake --build --preset conan-debug`, then
    `ctest --preset conan-debug -j 6` (run long suites in the background; do not
    edit service sources while it runs — the service fingerprint check fails).
  - UI: `pnpm exec tsc --noEmit -p apps/clients/terminal/tsconfig.json`,
    `pnpm lint`, `pnpm test:unit`.
  - E2E only through `pnpm run test:e2e` or `pnpm run test:e2e:journeys`.
- Report what changed, what was tested, and what was not verified.
- The current refactoring plan and its status:
  [docs/reviews/project-assessment-2026-10-25.md](docs/reviews/project-assessment-2026-10-25.md).
