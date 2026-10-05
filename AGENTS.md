# Repository Instructions

Read this file before working. It applies to the entire repository and contains
only current rules. Project requirements below remain mandatory; the engineering
philosophy guides implementation within those constraints.

## Scope

- Build, test, and ship **macOS Terminal for futures only**.
- Do not add Web, Mobile, Notebook, standalone CLI, or Windows/Linux desktop clients.
- Remote services support **Linux x86_64** only and exist to serve Terminal.
- Build interfaces and implementations only for actual requirements.

## Current Implementation Only

- Keep one current implementation. Remove superseded code, contracts, configuration,
  scripts, tests, and documentation; do not retain reference or backup implementations.
- No legacy interfaces, protocols, aliases, compatibility layers, parallel
  implementations, migration entry points, or downgrade paths. Update callers,
  tests, and documentation together.
- Reject unsupported inputs and persisted formats. Version checks reject unsupported
  formats; they do not restore old implementations. Preserve user data.

## Architecture

- Core layers, lowest to highest: `asterion_foundation`, `asterion_kernel`,
  `asterion_domain`. Dependencies point downward only.
- Core provides mechanisms. Providers, storage backends, strategies, and risk
  algorithms belong in `plugins/`. Plugins are trusted in-process code, not a sandbox.
- Each service is a separate process managed by Node Agent. Closing an installed
  Terminal window leaves services running. Exiting the single development entry
  point stops local services and Agent, preserves data, and leaves remote nodes running.
- IPC uses Protobuf from `protocol/proto/`: Unix sockets locally, TCP + mTLS remotely.
- Terminal C++ orchestration belongs in `apps/clients/terminal/native/`;
  UI plugin contracts belong in `apps/clients/terminal/plugins/contract.ts`.

## Implementation

- Use C++20, RAII, and explicit ownership. Exceptions must not cross C ABI or Node-API boundaries.
- Use `Decimal` for money and prices; binary floating point is not authoritative ledger data.
- Use typed domain interfaces. Restrict JSON to protocol boundaries and UI presentation.
- Core and services emit English diagnostics; cross-process errors carry `ErrorCode`.
  Register new user-facing diagnostics in `apps/clients/terminal/src/i18n/locales/diagnostics.*.json`.
- Write durable state and keys through `kernel/durable_file.hpp`.
- Bump the log engine identifier in `apps/services/trading/live_session.cpp` when
  risk or trading-command semantics change. Bump the backtest engine version when
  matching, fees, or margin semantics change. Reject mismatched identifiers on recovery.
- Lock dependencies with Conan and pnpm. No implicit downloads or global include/link
  paths. Use spdlog, CLI11, and GoogleTest + CTest.
- Follow `.clang-format` and Prettier. Run `pnpm run format` before committing.

## Data and Trading Safety

- Market and historical data come from data providers. Do not substitute local
  CSV/JSON imports or fabricate market data. Fixtures are for tests only.
- Live trading requires authorization, account risk controls, and the unified
  execution path. Reject execution if any part is missing.
- CTP accounts and backtests require risk configuration. Missing configuration or
  unavailable risk plugins must reject execution, never allow it by default.
- Never automatically resend trading commands after a disconnect.
- Enter or generate credentials locally. Never put them in source, logs, or chat.
  Users may explicitly save market-data login passwords and authorization codes
  in the macOS Keychain. Never put them in ordinary configuration files or UI
  storage, and never reuse saved market credentials for trading login.
- Never silently rewrite, migrate, or delete user ledgers, tasks, or datasets.

## UI

- Follow the minimal, technical, futuristic style in [Terminal design](docs/terminal.md).
- Use Tonghuashun Futures as the market-layout reference. Clearly mark unavailable
  data sources; do not fabricate content.
- Localize UI text through `src/i18n`. Update English and Chinese together;
  missing keys must fail explicitly.

## Workflow

- Work directly on `main` in the primary checkout. Do not create development
  branches unless requested. Do not push unless requested.
- Keep audits and reviews in `docs/reviews/`. Before related work, read the
  [technical audit](docs/reviews/technical-audit.md) and
  [remediation progress](docs/reviews/audit-progress.md), compare their baselines
  with current code, and update progress after completing work.
- For running desktop issues, verify the actual launch directory, frontend source,
  and native-module path. See [connection conflicts](docs/reviews/connection-conflict.md)
  and [checkout integration](docs/reviews/worktree-gap.md). Never transfer test
  conclusions between checkouts without verifying the inputs.
- Run E2E only through `pnpm run test:e2e`, which isolates Agent and the test CTP SDK.
  Never invoke `playwright test` directly; it can operate real local services.
- Changes to `core/`, `protocol/`, `plugins/`, `apps/services/`, or `bindings/`
  change the Linux service source fingerprint. Rebuild before release following
  the [development guide](docs/development.md); `pnpm desktop` only warns in development.
- Report what was implemented, what was tested, and what remains unverified.

## Engineering Philosophy

Optimize for correctness, simplicity, architectural coherence, and long-term
maintainability.

Do not optimize for maximum defensive coverage, maximum test count, or maximum
abstraction.

Prefer the smallest correct design that satisfies the actual requirements and
preserves the system's intended invariants.

A solution is not better merely because it handles more hypothetical cases.

### Core Principles

1. Prefer simple designs over defensive complexity.

2. Fix problems at their architectural root rather than adding local patches.

3. Enforce invariants at the correct boundary instead of repeatedly checking
   them throughout the codebase.

4. Prefer explicit failure over speculative recovery.

5. Add fallback, retry, validation, compatibility, or recovery behavior only
   when there is a concrete and justified failure mode.

6. Do not design for hypothetical future requirements unless the current task
   explicitly requires them.

7. Prefer deleting, simplifying, or restructuring incorrect abstractions over
   adding more code around them.

8. Avoid abstractions that have only one real use case unless they materially
   improve correctness or clarity.

9. Minimize the number of concepts a developer must understand to reason about
   the system.

10. Preserve clear ownership of state and responsibility. Avoid duplicating
    state or responsibility across layers.

### Avoid Speculative Defensive Programming

Do not add defensive behavior merely because a state is theoretically possible.

Before adding any defensive branch, validation, fallback, retry, or recovery
mechanism, identify:

- the concrete failure mode,
- whether that failure mode is actually reachable,
- the architectural boundary responsible for preventing or handling it,
- and why the proposed behavior is preferable to failing clearly.

Do not add:

- speculative validation,
- speculative fallback paths,
- retries without a demonstrated transient failure mode,
- compatibility layers for hypothetical callers,
- redundant null or state checks for invariants guaranteed elsewhere,
- silent recovery from invariant violations,
- catch-all exception handling that hides programming errors,
- defensive copies without a demonstrated ownership or mutation problem,
- generic infrastructure created solely for one narrow use case.

If an invariant should never be violated, make that invariant explicit and fail
clearly when it is violated.

Do not silently turn programmer errors into recoverable runtime states unless
the architecture explicitly requires that behavior.

### Solve Root Causes, Not Symptoms

When fixing a bug, first determine why the system allowed the bug to exist.

Do not immediately patch the location where the symptom appears.

Ask:

- Is responsibility assigned to the wrong component?
- Is the abstraction incorrect?
- Is state represented in more than one place?
- Is an invariant missing or enforced too late?
- Is lifecycle ownership unclear?
- Is the API permitting invalid states?
- Is the current implementation fundamentally more complicated than necessary?

Prefer a structural fix when it removes an entire class of failure.

A slightly larger root-cause fix is preferable to a sequence of small patches
when the patches preserve a flawed design.

### Stop-and-Rethink Rule

Stop incremental patching and reassess the design if any of the following
occurs:

- the same component requires more than two corrective iterations,
- multiple special cases begin accumulating,
- one fix repeatedly exposes adjacent edge cases,
- several fallback paths are needed for a simple operation,
- tests become substantially more complicated than the production behavior,
- a small feature requires several new abstractions,
- state must be synchronized between multiple owners,
- correctness depends on many scattered defensive checks,
- the implementation keeps growing without making the core model simpler,
- a change is technically passing tests but becoming harder to explain.

When this happens:

1. Stop modifying code.
2. Restate the underlying problem.
3. Re-evaluate the relevant abstraction and ownership boundaries.
4. Consider whether existing code should be removed or simplified.
5. Compare at least two alternative designs.
6. Resume implementation only after identifying the simplest coherent design.

Do not continue a patch-test-patch loop indefinitely.

### Design Before Implementation

For non-trivial changes, reason about the design before editing production code.

Before implementation, establish:

- the root problem,
- the relevant architectural boundary,
- the invariants that must hold,
- the minimal behavior required,
- explicit non-goals,
- the expected data/control flow,
- failure semantics,
- ownership and lifecycle,
- and what existing code can be simplified or removed.

For significant architectural changes, consider at least two viable designs and
prefer the one with fewer concepts, fewer states, fewer special cases, and
clearer ownership.

Do not introduce a generalized framework when a direct implementation is
sufficient.

Do not expand task scope merely because adjacent improvements are possible.

### Scope Discipline

Implement only what is required for the current task plus changes necessary to
preserve correctness and architectural consistency.

Do not opportunistically:

- redesign unrelated modules,
- introduce generic infrastructure,
- add future-facing extension points,
- support hypothetical deployment modes,
- add compatibility behavior without a current consumer,
- refactor unrelated code solely because it could be cleaner,
- or convert a local requirement into a system-wide framework.

If an adjacent issue is important but not required, document it separately
instead of expanding the current change.

### Testing Philosophy

Tests exist to establish confidence in meaningful behavior and important
invariants.

The goal is not maximum line coverage, branch coverage, test count, or exhaustive
enumeration of theoretically possible states.

Prefer a small number of high-value tests over a large number of low-value
tests.

Prioritize tests for:

- primary externally observable behavior,
- important system invariants,
- meaningful boundary conditions,
- previously observed regressions,
- realistic failure modes,
- state transitions that could corrupt or lose data,
- concurrency behavior where concurrency actually exists.

Avoid tests that:

- assert private implementation details,
- duplicate the same behavior through many parameter combinations without added
  risk coverage,
- test impossible states,
- exist only to exercise a line or branch,
- mock so much of the system that they no longer validate useful behavior,
- lock in an implementation that should remain free to change,
- test defensive code that should not exist in the first place.

Do not add a regression test for every incidental implementation mistake.

Add a regression test when the failure represents behavior that could plausibly
recur and is important enough to protect.

When tests fail after a change, first determine whether:

- the implementation is wrong,
- the test encodes an obsolete assumption,
- or both reflect a deeper design problem.

Do not automatically modify production code merely to satisfy an existing test.

### Test Budget

For ordinary changes, start with the minimum useful set:

- the main success path,
- important boundary behavior,
- concrete regression cases,
- and meaningful failure behavior.

Expand coverage only when additional tests protect a distinct and realistic
risk.

Do not create exhaustive combinatorial coverage unless the domain genuinely
requires it.

If the test suite added for a simple feature becomes larger or conceptually more
complex than the feature itself, reconsider both the design and the testing
strategy.

### Failure Handling

Distinguish between:

- expected runtime failures,
- transient external failures,
- invalid user input,
- corrupted external data,
- violated internal invariants,
- and programmer errors.

Handle each category deliberately.

Expected runtime failures may require explicit handling.

Transient failures may justify retry behavior when retry semantics are safe and
well-defined.

Invalid external input should be rejected at the appropriate boundary.

Internal invariant violations should normally fail loudly rather than be hidden
behind fallback behavior.

Do not use broad exception handling as a substitute for understanding failure
semantics.

Never retry an operation unless it is known to be safe or idempotent, or the
retry mechanism explicitly prevents duplicated effects.

### Abstraction Discipline

Introduce an abstraction when it removes meaningful duplication, establishes a
real architectural boundary, or makes an important invariant easier to enforce.

Do not introduce abstractions merely to make code appear extensible.

Prefer:

- concrete code over premature frameworks,
- explicit control flow over excessive indirection,
- composition over unnecessary hierarchy,
- domain-specific interfaces over generic abstractions,
- fewer layers when additional layers add no real boundary.

Every new abstraction should answer:

- What complexity does this remove?
- What invariant does this protect?
- What independent variation does this enable today?
- Why is direct code insufficient?

If those questions do not have convincing answers, prefer the simpler design.

### Code Size and Deletion

Treat deletion as a first-class engineering tool.

When modifying a subsystem, actively look for:

- obsolete branches,
- redundant validation,
- duplicated state,
- superseded abstractions,
- dead compatibility code,
- unnecessary wrappers,
- and tests that only protect removed behavior.

Prefer:

    +120 lines
    -400 lines

over:

    +800 lines
    -20 lines

when both solve the same problem correctly.

Do not preserve complexity merely because it already exists.

### Local vs Global Optimization

Do not optimize only for the current failing test, function, or call site.

After identifying a local fix, ask whether it improves or harms the global
system design.

A locally convenient fix should be rejected if it:

- weakens architectural boundaries,
- duplicates responsibility,
- creates another source of truth,
- makes ownership less clear,
- increases the number of possible states,
- or creates future synchronization requirements.

When repeated local fixes are necessary, assume the abstraction may be wrong
until proven otherwise.

### Implementation Review

Before considering a non-trivial change complete, review the resulting design
and ask:

- Is there a simpler solution?
- Did we solve the root cause?
- Did we add speculative defensive behavior?
- Are all fallbacks justified by concrete requirements?
- Are retries justified and safe?
- Did we introduce unnecessary abstractions?
- Did we duplicate state or responsibility?
- Are ownership and lifecycle clear?
- Are tests focused on meaningful behavior?
- Are tests coupled to implementation details?
- Can any new production code be deleted while preserving correctness?
- Can any old code now be removed?
- Did the implementation remain within scope?
- Does the resulting architecture become easier, rather than harder, to explain?

Passing tests is necessary but not sufficient.

A change is complete only when it is correct, appropriately tested, and
architecturally simpler or at least no more complicated than necessary.

### Decision Priority

When engineering goals conflict, use this order of preference:

1. Correctness
2. Clear invariants and ownership
3. Simplicity
4. Architectural coherence
5. Maintainability
6. Observability and diagnosability
7. Appropriate testing
8. Performance where relevant
9. Defensive behavior only where justified
10. Generality and extensibility only when currently required

Do not sacrifice the first six items merely to maximize the last four.

### Default Bias

When uncertain between two correct approaches, prefer the one with:

- fewer states,
- fewer branches,
- fewer abstractions,
- fewer dependencies,
- fewer hidden side effects,
- fewer recovery paths,
- clearer ownership,
- more explicit invariants,
- and less code.

Simple does not mean simplistic.

The objective is the smallest design that correctly represents the actual
problem.
