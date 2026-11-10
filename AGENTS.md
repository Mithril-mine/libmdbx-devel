
## Testing instructions
 - use CMake for build and CTest for testing
 - use at least Linux and Windows both environments for build and testing

## Code style
 - use LLVM codestyle

## ChangeLog rules

The format follows [Keep a Changelog 2.0.0](https://keepachangelog.com/en/2.0.0/)
with the house deviations listed here.

Scope and curation:

- Only user-affecting changes (reaching the release build) go into
  `ChangeLog.md`; curate, don't accumulate — the ChangeLog is a summary of
  notable changes for readers, not a record of commits.
- One cause — one entry: related fixes from one issue are a single entry
  listing the affected APIs, not a line per function.

Structure:

- A version section uses only the Keep-a-Changelog types: `Added`,
  `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`, plus the house
  `Appreciations` section. No other top-level sections; an empty type is
  omitted.
- Every entry begins with the verb of its type: `Added …`, `Changed …`,
  `Deprecated …`, `Removed …`, `Fixed …`; a `Security` entry begins with
  its CVE identifier when present.
- `Fixed` keeps thematic subsections; subsections exist only inside
  `Fixed`, follow the established taxonomy and stay few and stable.
- A version opens with a summary of one or two plain sentences: what the
  release gives the user.
- Breaking changes are marked `**Breaking:**` on the entry itself, within
  its type — never collected into a separate section.
- The version being prepared is the upcoming version's section with the
  stage placeholder (e.g. `at/in a release engineering stage`, optionally
  with an `LTS` prefix); a released version is `## vX.Y.Z at YYYY-MM-DD`.

Wording:

- User-facing wording: no internal jargon, no low-level details — describe
  what the user would have observed.
- An entry is a single sentence of up to 100 characters, not counting the
  trailing issue links. Longer entries only for large reworks and only
  with the owner's direction. Every entry must be semantically complete
  and end with a period.
- Backports get ` (backport)` immediately before the final period.
- Issues must be referenced only as markdown links; bare `#N` mentions
  are noise.

House deviations from Keep a Changelog: the `Appreciations` section;
thematic subsections inside `Fixed`; issue links inline at the end of
entries; the stage placeholder instead of `[Unreleased]`.
