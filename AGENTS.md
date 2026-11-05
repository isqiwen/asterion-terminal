# Repository Instructions

Rules for anyone, human or AI, who changes this repository. Keep this file short.

## Product and plan

- One product: a futures quant Terminal for one person, on macOS (Apple Silicon) and Linux x86_64. Market and Trading are Linux services that serve it. No web, mobile, notebook, standalone CLI or Windows clients.
- What the product is: [docs/product.md](docs/product.md). What to build, in what order, and when it is done: [docs/plan.md](docs/plan.md). Take the next open work package from its order table and do only that.
- Trading rules: [docs/safety.md](docs/safety.md). Read them before changing anything that can send or cancel an order. Never remove code that enforces one of them.
- A phase is done only when it has worked for real, on real data or the CTP simulation counter. Passing fixture tests is necessary, not sufficient.
- The project is unreleased. Keep one current implementation: no legacy interfaces, aliases, migrations, version counters or backup copies. Git holds history.

## Architecture

- C++ owns market data and execution; Python owns strategies, factors and research. Strategies return target lots or weights; the C++ account turns them into orders.
- **Market** (`apps/services/market/`): one C++ process on a Linux host for real-time feeds, recording, historical acquisition and the versioned warehouse. All clients share one dataset; one client's subscriptions or disconnects never affect another.
- **Lab** (`apps/services/lab/`): the local Python research service that Terminal starts and restarts. It reads fixed Market versions into a disposable cache; `engine/` exposes core's replay to Python.
- **Trading** (`apps/services/trading/`): one C++ process per account on a Linux host, plus one Python child process per strategy instance. It runs without Terminal, Lab or Market. Closing Terminal never stops trading; a Trading restart never restores authorization. Strategy instances use only market data Trading itself received.
- **Terminal** (`apps/clients/terminal/`): Electron + React UI (`src/`, `features/`) with a C++ native layer (`native/`). It starts only Lab. Pages build and read the generated Protobuf types; the native layer only holds the TLS connections and relays them.
- `core/`: `asterion_base` → `asterion_system` → `asterion_futures`; dependencies point down. Shared service code lives in `apps/services/common/`; the strategy API is `apps/services/common/python/asterion_strategy.py`.
- IPC is Protobuf from `protocol/proto/`: Unix sockets locally, mutual TLS with pinned certificates to Market and Trading.
- Services only read configuration; missing or invalid configuration fails with its path. Defaults live in `config/` and in each service's setup script. Credentials live only in systemd encrypted credentials on the service host.

Details: [docs/architecture.md](docs/architecture.md).

## Code style

Write the smallest change that does the job, in plain code a reader understands at once. Reviewers delete anything that has no reason to exist.

- **Validate at the edges only**: network and protocol input, files, user input, broker and provider callbacks. Inside a process, trust callers and types; do not re-check what a caller or a type already guarantees.
- **No speculative safeguards**: every check, fallback, retry, timeout or limit must name a concrete situation that can actually happen. If you cannot name one, leave it out. If unsure, leave it out and mention it in your report.
- **Let errors propagate**: catch only where you can do something useful (show the user, release a resource, keep a service running). No catch-and-rethrow with a new message.
- **No new machinery unless the work package asks for it**: state machines, generation counters, permits, budgets, extra threads, queues, registries, caches or digests.
- **Comments** only when the reason is not obvious, one line. Never restate the code, list what it does not do, or narrate history.
- **Tests**: one test per behaviour a user would notice, at the lowest layer that shows it. No tests for guards, impossible states or private helpers.
- **Docs** (Chinese, in `docs/`) say what the user can do and see, plus design decisions with a one-line reason. No thread counts, queue sizes or other details the code already shows; no "this does not prove or guarantee" disclaimers; say each thing once. Update a doc in the commit that changes its behaviour. Process notes go in commit messages, not docs.
- **Before finishing**, reread your diff and delete every line that neither changes behaviour nor helps a reader. A fix is usually a few lines; if yours is not, say why in the commit message.
- Older code here is often more defensive than these rules. Do not copy that style. Simplify what you touch within the work package, but never weaken a rule in `docs/safety.md`.

Example: the caller already validated `weights` when it parsed the request.

```cpp
// Avoid
if (weights.empty() || weights.size() > 20)
    throw std::invalid_argument("invalid weights");
try {
    targets = size_targets(equity, weights);
} catch (const std::exception& error) {
    throw std::runtime_error(std::string("sizing failed: ") + error.what());
}

// Prefer
targets = size_targets(equity, weights);
```

## Engineering

- C++20 with RAII. `Decimal` for prices, quantities and money. Its `*` is exact and throws when the result needs rounding; when both operands can have fractional digits, call `multiply` with an explicit `Rounding`.
- Exceptions never cross C ABI or Node-API boundaries. JSON only at protocol and UI boundaries.
- Durable files go through `system/durable_file.hpp`.
- CMake targets live in their module's `CMakeLists.txt`; names use underscores. Dependencies are locked with Conan and pnpm.
- Format with `pnpm run format` (clang-format 23.1.1 and Prettier).
- UI text goes through `src/i18n`; Chinese and English change together.
- Avoid unnecessary secondary labels and descriptions. Let the layout do the explaining.
- Market and historical data come from real providers; fixtures are only for tests.

## Workflow

- Work on `main`. Commit each finished work package, or a coherent step of one; never push unless asked.
- Commit subjects start with the work package ID, for example `M3.7: ...`. When a work package is done, update its row in the order table of `docs/plan.md`.
- Build and test:
  - C++: `cmake --build --preset conan-debug`, then `ctest --preset conan-debug -j 6` (run long suites in the background). `-L unit` runs only in-memory tests (about 20 s); `-L process` the rest.
  - Lab Python tests are registered only after `pnpm prepare:python`.
  - UI: `pnpm exec tsc --noEmit -p apps/clients/terminal/tsconfig.json`, `pnpm lint`, `pnpm test:unit`.
  - E2E only through `pnpm run test:e2e` or `pnpm run test:e2e:journeys`.
- Steps that need a real broker account, real credentials, signing or a production deployment belong to the user.
- Report what changed, what was tested, and what was not.
