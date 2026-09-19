# Memory-of-Code Codex (Kaizen-based)

> Operational rules for using and maintaining the knowledge graph about this codebase
> (memory-MCP: entities + relations). The codex itself is subject to continuous
> improvement — it is a *standard*, and per Kaizen a standard exists to be improved.

## Purpose

The knowledge graph is an **index and hypothesis store**, not a source of truth.
It persists architectural insight, invariants, naming traps and cross-module links
across agent sessions. Source code remains the only source of truth.

**Golden rule (Gemba):** before relying on any observation when modifying code,
verify it against the real source with a targeted `grep`/`read`. If it disagrees,
trust the code, update the observation immediately.

## Kaizen principles mapped to memory work

| Kaizen principle | Application to the memory-of-code process |
|---|---|
| **Gemba** («go and see») | Verify every observation against actual code before acting on it; never edit code solely from memory. |
| **PDCA cycle** | Plan (decide what to record) → Do (write observations after task completion) → Check (review freshness/accuracy against code) → Act (fix stale entries, refine the codex). |
| **Small incremental steps** | Record observations as small, self-contained facts with provenance (symbol / file), not monolithic essays; update atomically per task. |
| **Muda (waste) elimination** | Avoid re-scanning when the graph is fresh; avoid recording transient state (uncommitted diffs, temp paths, «how-to» steps); prefer updating an existing observation over appending duplicates. |
| **Root cause (5 Whys)** | Capture *why* and invariants (the `key_caveats` style), not just *what*; note the reasoning so future readers don't re-derive it. |
| **Standardize, then improve** | Keep the format of observations and entity naming consistent; improve the standard itself through PDCA (log changes here). |
| **Measure** | Track the health of the graph: rate of stale/corrected observations, frequency of useful hits vs wasted re-scans; tune triggers accordingly. |

## When to write (triggers)

Write/update only after a **completed, verifiable unit of work**:

- a feature, refactor or fix is done (code compiles, tests pass, ideally committed);
- a non-obvious invariant or naming trap was discovered;
- a module's structure or key API surface changed.

Do not record: half-done work, speculative plans, session-specific artifacts,
secrets, absolute paths outside the repo, or vendored/third-party internals.

## Observation format

Each observation should be one factual statement:

```
<WHAT> (<where: symbol/file:line>) [since <commit-ish>] — <why / invariant / caveat>
```

- State version-sensitive facts with the commit/PR they refer to.
- Mark traps explicitly (they are the highest-value entries).
- Keep observations short enough to be verified in one glance.

## Freshness discipline

1. **Verify-then-act:** any observation used to justify a code change must be
   confirmed by a targeted search first.
2. **Update-on-mismatch:** if verification contradicts the graph, correct the
   observation right away — accuracy beats completeness.
3. **Refresh after large tasks:** after finishing a significant piece of work,
   audit the affected entities (e.g. module, api surface, build system, tests)
   and delete stale observations rather than layering new ones over them.
4. **Stale-flag instead of re-derive:** if you cannot refresh immediately,
   mark the observation stale in its text; never silently rely on it.

## Naming & relations

- Entity names: `Module` (architectural units), `DataStructure`, `Concept`,
  `Project`. Prefer stable, discoverable names; reuse existing entities over
  creating near-duplicates.
- Relations: connect module→subsystem, type→module, module→guards(module),
  in active voice (`wraps`, `builds`, `guards`, `implements`). Do not duplicate
  content across both endpoints — put detail in one observation and reference it.

## Kaizen loop for the codex itself

After every few sessions (or when a process friction repeats), revisit this file:

- identify waste in the memory workflow (redundant scans, stale reads, missed writes);
- propose one small change to the rules above;
- apply it, record the change, and note the expected effect.

Continuous improvement applies to memory-of-code as much as to code.