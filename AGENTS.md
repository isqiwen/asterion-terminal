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

- C++ owns market data and execution: real-time feeds, recording, historical
  acquisition and warehouse access, account processes, risk, the backtest
  ledger and matching. Python owns strategies, factors and research.
- Strategies output target positions or weights and never send orders. The C++
  kernel converts targets into orders and enforces [docs/safety.md](docs/safety.md).
- Work only on the active phase. Parts the plan replaces (the Node-API layer
  and hand-written JSON mappings) get the fixes the active phase needs, never
  new features.
- Every new mechanism serves a user scenario. Do not add digests, evidence
  records, validators or compatibility layers without one.
- A phase is done when it worked for real, on real data or the CTP simulation
  counter. Passing fixture tests is necessary, not sufficient.

## Target architecture (confirmed 2026-11-01)

- Remote Market is one C++ service process owning real-time feeds, recording,
  historical acquisition and the authoritative warehouse. It uses bounded
  threads and queues for acquisition, reads and writes; no Python runtime or
  acquisition subprocess. Market and Trading are independent Linux deployments,
  never owned by a client. Market serves multiple clients over one authoritative
  dataset; selections, cursors and disconnects belong to each client and must not
  change another client or the service collection lifecycle. Market C++ owns
  acquisition and warehouse writes. Lab owns research and results, with fixed
  version reads and disposable read-only caches. Provider credentials belong
  on the Market host.
- Terminal publishes small, versioned strategy code packages and instance
  configuration. Trading loads deployed packages on its Linux host, manages
  Python strategy child processes and connects directly to market and trading
  fronts. No execution dependency on Terminal, Lab or central Market.
- Live strategies warm up only on complete bars received after each instance
  starts. No historical preload, supplied warmup data or reused old window.
  The strategy reports readiness from its parameters and state; no strategy
  orders before readiness. Account positions and pending orders remain owned
  by Trading through strategy restarts.
- Closing Terminal never stops or pauses authorized trading. Trading restart
  still does not restore authorization. M3.0/M3.1 and M3.2 execution code and
  isolated validation are complete; real-provider acceptance remains pending.
  The active work is M3.3 instance operations and monitoring, then M3.4 client cleanup.
  Ready targets use direct feed quotes and explicit weight budgets through the
  account execution path; real-counter acceptance remains pending.

## Architecture (current code)

- Core layers, lowest first: `asterion_base` → `asterion_system` →
  `asterion_futures`. Dependencies point down. Implementation belongs to its owning application;
  genuinely shared service code lives in `apps/services/common/`. Shared database
  access and built-in order limits live in `core/`. Market C++ sources live in
  `apps/services/market/acquisition/`; Lab only reads fixed Market versions through
  `apps/services/lab/asterion_lab/market.py` and keeps disposable read-only caches. Strategies are Python classes and
  factors plain Python functions in the user's strategy repository, not a
  separate plugin system.
- Business services (`apps/services/`): Market (remote feeds, recording, C++ warehouse and versioned reads; Tushare/exchange acquisition and recorded-minute publication through persistent tasks, authenticated acquisition management, host credentials and Terminal data-page access implemented), Lab (Python: fixed Market reads, read-only caches and research runs; `lab/engine` is the core replay
  as a Python extension) and Trading (one process per account, immutable strategy packages and a resource-limited Python child; its independent offline runtime ships in the Linux release and upgrades require all accounts stopped; direct CTP strategy bars, live-only warmup, fixed target inputs and serial execution through the account path; runner/account lifecycle and fault cancellation are implemented). The shared strategy API is `apps/services/common/python/asterion_strategy.py`. Each owns its
  state. There is no central task service or worker process.
- Local services: Terminal starts only Lab as its child process (`LocalServices`
  in `apps/clients/terminal/native/`), restarts it on failure and it exits with
  Terminal. Market and Trading are independent Linux installations supervised
  by systemd. Terminal talks to each service directly.
- IPC: Protobuf from `protocol/proto/`; Unix sockets locally; Market and Trading require pinned mutual TLS
  on every remote connection. Market assigns read/manage roles to registered client certificates.
- Terminal: Electron + React UI (`apps/clients/terminal/src`, `features/`), C++
  orchestration in `apps/clients/terminal/native/`. The installer is in
  `apps/clients/installer/`.
- Configuration: defaults in repository `config/`, copied once at deployment to
  `<node>/config/`. Independent Trading and Market setup scripts own their
  defaults and generate actual configuration at installation; no default JSON
  files are shipped. Trading creates `config/accounts/<account>.json`, Market
  creates `config/market.json`, and both create `config/logging.json`. Upgrades
  preserve existing configuration. Services only read configuration;
  missing or invalid configuration fails with its path.

Details: [docs/architecture.md](docs/architecture.md).

## Trading and data safety

The full contract is [docs/safety.md](docs/safety.md). Its core:

- Market and historical data come from real providers. Never fabricate market
  data; fixtures are for tests only.
- Live trading requires authorization, account risk controls and the single
  execution path. Missing or invalid risk configuration rejects execution. Risk rules are compiled
  into Trading and the backtest engine; ledgers record policy parameters and changes, never algorithm copies.
- Never resend a trading command automatically after a disconnect or an unknown
  outcome.
- Credentials are entered locally and never stored in source, logs, chat,
  configuration files or UI storage. Market CTP and historical provider credentials are entered on the Market host and stored as systemd
  encrypted credentials. Trading credentials are entered
  on the trading host and stored per account as systemd encrypted credentials;
  Terminal never receives them.
- Never silently rewrite, migrate or delete user ledgers, tasks or datasets.

## Engineering

- C++20 with RAII; `Decimal` for money and prices; exceptions never cross C ABI
  or Node-API boundaries. JSON only at protocol and UI boundaries.
- Durable files go through `system/durable_file.hpp`.
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
