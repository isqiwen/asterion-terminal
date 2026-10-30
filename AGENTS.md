# Repository Instructions

Rules for anyone changing this repository. Keep this file short and current.

## Scope

- One product: the futures **Terminal** on macOS (Apple Silicon) and Linux x86_64.
  Remote services run on Linux x86_64 only and exist to serve Terminal.
- No Web, mobile, notebook, standalone CLI or Windows clients.
- The project is unreleased. Keep one current implementation: no legacy
  interfaces, aliases, migration paths, version counters or backup copies of code.
  Git holds history.

## Direction

What the product is: [docs/product.md](docs/product.md). The route, the active
phase and its work packages:
[docs/reviews/project-assessment-2026-10-28.md](docs/reviews/project-assessment-2026-10-28.md).

- C++ owns money and orders: market gateway and recording, account processes,
  risk, the backtest ledger and matching. Python owns ideas: strategies,
  research and data acquisition.
- Strategies output target positions or weights and never send orders. The C++
  kernel converts targets into orders and enforces [docs/safety.md](docs/safety.md).
- The user-approved M1.9 consolidates extension boundaries and engineering structure.
  Work only on the active phase. Parts the plan replaces (Data, Research, the
  shared task store, the C++ rule strategies, Node Manager and SSH
  orchestration, the Node-API layer and hand-written JSON mappings) get the
  fixes the active phase needs, never new features.
- Every new mechanism serves a user scenario. Do not add digests, evidence
  records, validators or compatibility layers without one.
- A phase is done when it worked for real, on real data or the CTP simulation
  counter. Passing fixture tests is necessary, not sufficient.

## Architecture (current code)

- Core layers, lowest first: `asterion_foundation` → `asterion_kernel` →
  `asterion_domain`. Dependencies point down. Implementation belongs to its owning application;
  genuinely shared service code lives in `apps/services/common/`. Shared database
  access and built-in order limits live in `core/`; third-party data-source
  implementations live in `extensions/`. Python strategy extension is M2.1. Factors are reusable
  research functions/packages, not a separate plugin system.
- Business services (`apps/services/`): Market, Data, Research and Trading
  (one process per account). Each owns its state and runs its own tasks
  in bounded thread pools. There is no central task service or worker process.
- Management (`apps/management/`): Node Manager (on the Terminal machine) owns
  node registration and Agent connections; Node Agent (on every node) supervises
  that node's service processes. Both are supervised by the OS service manager.
  Trading and Recorder are independent Linux installations supervised by systemd.
  Neither management process proxies business traffic: Terminal connects directly.
- IPC: Protobuf from `protocol/proto/`; Unix sockets locally, plain TCP with the
  `ASTR` preface on the LAN, and pinned mutual TLS for all Trading connections.
- Terminal: Electron + React UI (`apps/clients/terminal/src`, `features/`), C++
  orchestration in `apps/clients/terminal/native/`. The installer is in
  `apps/clients/installer/`.
- Configuration: defaults in repository `config/`, copied once at deployment to
  `<node>/config/`. Independent Trading and Recorder setup scripts own their
  defaults and generate actual configuration at installation; no default JSON
  files are shipped. Trading creates `config/accounts/<account>.json`, Recorder
  creates `config/recorder.json`, and both create `config/logging.json`. Upgrades
  preserve existing configuration. Services only read configuration;
  missing or invalid configuration fails with its path.

Details: [docs/architecture.md](docs/architecture.md).

## Trading and data safety

The full contract is [docs/safety.md](docs/safety.md). Its core:

- Market and historical data come from real providers. Never fabricate market
  data; fixtures are for tests only.
- Live trading requires authorization, account risk controls and the single
  execution path. Missing or invalid risk configuration rejects execution. Risk rules are compiled
  into Trading and Research; ledgers record policy parameters and changes, never algorithm copies.
- Never resend a trading command automatically after a disconnect or an unknown
  outcome.
- Credentials are entered locally and never stored in source, logs, chat,
  configuration files or UI storage. Saved market-data credentials live in the
  OS keychain and are never reused for trading. Trading credentials are entered
  on the trading host and stored per account as systemd encrypted credentials;
  Terminal never receives them.
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
    `ctest --preset conan-debug -j 6` (run long suites in the background).
    `-L unit` runs only in-memory tests (about 20 s); `-L process` the rest.
  - UI: `pnpm exec tsc --noEmit -p apps/clients/terminal/tsconfig.json`,
    `pnpm lint`, `pnpm test:unit`.
  - E2E only through `pnpm run test:e2e` or `pnpm run test:e2e:journeys`.
- Report what changed, what was tested, and what was not verified.
- Prefix commit subjects with the work package ID (for example `M0.3`) and
  update its row in the plan's status table.
- Docs follow code: update or delete a topic doc in the commit that replaces or
  deletes its code. Process notes belong in commit messages, not in docs.
