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

## Swarm operation: duty loop and command authority (2026-09-20)

Operational rules for how agents communicate, per the owner's ruling. These
belong in the codex because they govern agent behavior across sessions.

### Command authority
- **The coordinator (main_architect) is the command instance for the swarm.**
  Agents receive tasks, decisions and reviews FROM the coordinator, not from
  the owner. The owner does not participate in routine work and must not be
  asked for per-task commands.
- Owner involvement is reserved for escalation: priorities, conflicts between
  agents, changes of specialization, or decisions explicitly flagged
  `QUESTION escalate`. Everything else is decided by the coordinator within
  the protocol SLOs.

### Duty loop of the coordinator
- The coordinator keeps a self-driven duty loop:
  `waitmail main_architect` (blocks until a letter) → read and process ALL
  pending letters (answer, review, merge, close wait= records, update board)
  → run `waitmail main_architect` again.
- The owner interrupts the loop (`Ctrl+C` / a message) when the coordinator
  is needed personally. While the loop runs, the coordinator is continuously
  responsive to agents without waiting for the owner.
- Consequence: "check mail as the first step of every turn" is subsumed by the
  duty loop; the loop IS the first and permanent step while on duty.

### Agent side
- Agents send all requests/reports as letters to `skynet_inbox_main_architect`
  and wait for answers via their own `waitmail <slug>` — they do NOT wait for
  the owner, and they do NOT block on absent coordinator answers (protocol §20.9
  «не блокируйся» still applies to work steps).
- If the coordinator does not answer within the SLO, the agent escalates with
  `QUESTION escalate` — never by pinging the owner directly.
- Tasks assigned by the coordinator are authoritative; start them immediately
  and report via `REPORT`.

## Memory infrastructure & traps (2026-09-21 → 2026-09-22)

**The canonical store is now a libmdbx database, not a JSONL file.** The
JSONL-backed `@modelcontextprotocol/server-memory` was replaced by
`memory-mdbx-server.py` (ACID, inter-process safe via the engine's lock file).
This eliminated the lost-update race of the old read-modify-write JSONL store.

```
DB (canonical): ~/.local/share/libmdbx-memory/graph.mdbx   (+ -lck)
Server:         ~/.local/share/libmdbx-memory/memory-mdbx-server.py
Library:        ~/.local/share/libmdbx-memory/build/libmdbx.so   (master e77297e0)
Legacy JSONL:   ~/.local/share/libmdbx-memory/memory.jsonl       (archive; old sessions)
Reader bridge:  /sourcecraft/workspace/.skynet/memory_reader.py  (merge mdbx+legacy)
MCP config:     ~/.config/opencode/opencode.json -> mcp.memory
```

- **Schema**: 4 subdbs, dupsort = `key -> sorted_set_of_values`. `E` (meta,
  name → JSON), `O` (name → observations, DUP), `R` (from → `to\0type`, DUP),
  `RV` (to → `from\0type`, DUP). Limits @4K pagesize: key/dup-value ≤ 2022 bytes.
- **Tools** read via `memory_reader.py` (merges mdbx + legacy during transition);
  new sessions write to mdbx, sessions started before the switch keep appending
  to legacy JSONL until restarted.
- **Verify after critical writes**: `python3 memory-mdbx-server.py --dump-jsonl
  | grep <marker>` (the old npx-JSONL write could return success without landing).

### Trap 1: a second (docker) memory server exists but is NOT shared

`.codeassistant/mcp.json` defines `memory` as a **docker container**
(`mcp/memory`, volume `mcp-memory:/app/dist`). That stack is isolated and its
writes are invisible to the swarm. Everyone uses the global
`~/.config/opencode/opencode.json` → `memory-mdbx-server.py`. If a write "succeeds"
but does not appear in the DB, you likely hit the docker stack.

### Trap 2: old sessions still write to legacy JSONL

Until every opencode session is restarted, some agents write to the old
`memory.jsonl`. Always go through `memory_reader.py` so both sources merge.
After a full restart only `graph.mdbx` is live.

### Trap 3: never copy a live store with `cp`

Use `/sourcecraft/workspace/.skynet/migrate-memory.py` (parse+repair+validate).

### Provenance note

This is the 3rd+ iteration of debugging "why doesn't the swarm see my memory
write". Root cause: non-transactional JSONL + multiple writers → lost updates.
Fixed by switching the backend to libmdbx (owner decision 2026-09-22).

[Full workspace doc: `/sourcecraft/workspace/AGENT-WORKSPACE.md` → «Общая память
агентов (memory-MCP)»]