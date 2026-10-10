# libmdbx documentation

**libmdbx** is an embedded transactional key-value database: compact, extremely
fast (memory-mapped, B+tree, O(log N)), ACID, crash-safe by default, with no WAL
and no server process — safe for concurrent multi-process access.

Choose a section:

- **[Overview](guides/overview.en.md)** — what libmdbx is, its characteristics,
  comparison with alternatives, and when to choose it (plus the honest list of
  [restrictions and gotchas](guides/restrictions.en.md)).
- **[Getting started](guides/install-build.en.md)** — forms of delivery, how to
  build, [first steps](guides/first-steps.en.md) with the API, and
  [durability modes](guides/durability-modes.en.md).
- **[Textbook](textbook/en/README.md)** — a practical guide in six volumes,
  from the first steps to expert topics and bindings.
- **[Deep dive](engineering/architecture.en.md)** — the architecture, internal
  mechanisms, and what libmdbx improves over LMDB.
- **[Reference](api.en.md)** — the C/C++ API reference, command-line tools,
  and the change log.

The documentation is available in English and Russian — use the language pill
in the top-right corner of any page to switch.
