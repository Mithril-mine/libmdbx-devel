<!-- omit in toc -->
# Contributing to libmdbx

First off, thanks for taking the time to contribute! ❤️

All types of contributions are encouraged and valued. See the [Table of Contents](#table-of-contents) for different ways to help and details about how this project handles them. Please make sure to read the relevant section before making your contribution. It will make it a lot easier for us maintainers and smooth out the experience for all involved. The community looks forward to your contributions. 🎉

> And if you like the project, but just don't have time to contribute, that's fine. There are other easy ways to support the project and show your appreciation, which we would also be very happy about:
> - Star the **libmdbx** project
> - Tweet about it
> - Refer this project in your project's readme
> - Mention the project at local meetups and tell your friends/colleagues

<!-- omit in toc -->
## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [I Have a Question](#i-have-a-question)
- [Reporting Bugs](#reporting-bugs)
- [Suggesting Enhancements](#suggesting-enhancements)
- [Code Contribution](#code-contribution)

## Code of Conduct

This project and everyone participating in it is governed by the
[libmdbx Code of Conduct](https://sourcecraft.dev/dqdkfa/libmdbx-devel/browse/CODE_OF_CONDUCT.md?rev=master).
By participating, you are expected to uphold this code. Please report unacceptable behavior
to <libmdbx@dqdkfa.ru>.

## I Have a Question

> If you want to ask a question, we assume that you have read the available [Documentation](https://libmdbx.dqdkfa.ru/), including the [FAQ](https://libmdbx.dqdkfa.ru/#sec-faq) section.

Before you ask a question, it is best to search for existing issues at [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx-devel/issues) and [GitHub](https://github.com/Mithril-mine/libmdbx-devel/issues) that might help you.
In case you have found a suitable issue and still need clarification, you can write your question in this issue. It is also advisable to search the internet for answers first.

If you then still feel the need to ask a question and need clarification, we recommend the following:

- Ask in the [telegram group](https://t.me/libmdbx).
- Open an issue at [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx-devel/create-issue) or [GitHub](https://github.com/Mithril-mine/libmdbx-devel/issues/new).
- Provide as much context as you can about what you're running into.
- Provide project and platform versions (compiler, OS, etc), depending on what seems relevant.

We will then take care of the issue as soon as possible.

### Reporting Bugs

<!-- omit in toc -->
#### Before Submitting a Bug Report

A good bug report shouldn't leave others needing to chase you up for more information. Therefore, we ask you to investigate carefully, collect information and describe the issue in detail in your report. Please complete the following steps in advance to help us fix any potential bug as fast as possible.

- Make sure that you are using the latest version.
- Determine if your bug is really a bug and not an error on your side e.g. using incompatible environment components/versions (Make sure that you have read the [documentation](https://libmdbx.dqdkfa.ru/). If you are looking for support, you might want to check [this section](#i-have-a-question)).
- To see if other users have experienced (and potentially already solved) the same issue you are having, check if there is not already a bug report existing for your bug or error in the issues at [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx-devel/issues) and [GitHub](https://github.com/Mithril-mine/libmdbx-devel/issues).
- Also make sure to search the internet (including Stack Overflow) to see if users outside of the GitHub community have discussed the issue.
- Collect information about the bug:
  - Stack trace (Traceback)
  - OS, Platform and Version (Windows, Linux, macOS, x86, ARM)
  - Version of the interpreter, compiler, SDK, runtime environment, package manager, depending on what seems relevant.
  - Possibly your input and the output
  - Can you reliably reproduce the issue? And can you also reproduce it with older versions?

<!-- omit in toc -->
#### How Do I Submit a Good Bug Report?

> You must never report security related issues, vulnerabilities or bugs including sensitive information to the issue tracker, or elsewhere in public. Instead sensitive bugs must be sent by email to <libmdbx-security@dqdkfa.ru>.
<!-- You may add a PGP key to allow the messages to be sent encrypted as well. -->

We use GitHub issues to track bugs and errors. If you run into an issue with the project:

- Open an issue at [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx-devel/create-issue) or [GitHub](https://github.com/Mithril-mine/libmdbx-devel/issues/new). (Since we can't be sure at this point whether it is a bug or not, we ask you not to talk about a bug yet and not to label the issue.)
- Explain the behavior you would expect and the actual behavior.
- Please provide as much context as possible and describe the *reproduction steps* that someone else can follow to recreate the issue on their own. This usually includes your code. For good bug reports you should isolate the problem and create a reduced test case.
- Provide the information you collected in the previous section.

Once it's filed:

- The project team will label the issue accordingly.
- A team member will try to reproduce the issue with your provided steps. If there are no reproduction steps or no obvious way to reproduce the issue, the team will ask you for those steps and mark the issue as `needs-repro`. Bugs with the `needs-repro` tag will not be addressed until they are reproduced.
- If the team is able to reproduce the issue, it will be marked `needs-fix`, as well as possibly other tags (such as `critical`), and the issue will be left to be [implemented by someone](#your-first-code-contribution).

<!-- You might want to create an issue template for bugs and errors that can be used as a guide and that defines the structure of the information to be included. If you do so, reference it here in the description. -->


### Suggesting Enhancements

This section guides you through submitting an enhancement suggestion for libmdbx, **including completely new features and minor improvements to existing functionality**. Following these guidelines will help maintainers and the community to understand your suggestion and find related suggestions.

<!-- omit in toc -->
#### Before Submitting an Enhancement

- Make sure that you are using the latest version.
- Read the [documentation](https://libmdbx.dqdkfa.ru/) carefully and find out if the functionality is already covered, maybe by an individual configuration.
- Perform a search at [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx-devel/issues) and [GitHub](https://github.com/Mithril-mine/libmdbx-devel/issues) to see if the enhancement has already been suggested. If it has, add a comment to the existing issue instead of opening a new one.
- Find out whether your idea fits with the scope and aims of the project. It's up to you to make a strong case to convince the project's developers of the merits of this feature. Keep in mind that we want features that will be useful to the majority of our users and not just a small subset. If you're just targeting a minority of users, consider writing an add-on/plugin library.

<!-- omit in toc -->
#### How Do I Submit a Good Enhancement Suggestion?

Enhancement suggestions are tracked as issues at [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx-devel/issues) and [GitHub](https://github.com/Mithril-mine/libmdbx-devel/issues).

- Use a **clear and descriptive title** for the issue to identify the suggestion.
- Provide a **step-by-step description of the suggested enhancement** in as many details as possible.
- **Describe the current behavior** and **explain which behavior you expected to see instead** and why. At this point you can also tell which alternatives do not work for you.
- **Explain why this enhancement would be useful** to most libmdbx users. You may also want to point out the other projects that solved it better and which could serve as inspiration.

---

## Code contribution

We appreciate any contributions: bug fixes, new features, tests, documentation, examples, and benchmarks. Before starting, please read this guide.

### License & Copyright

The core of this project (the source code of the library itself) is distributed under the Apache‑2.0, but tests and related infrastructure are proprietary. By contributing code, you agree to the license terms.

The Contributor License Agreement (CLA) is under development, be ready to sign it. In short, you must agree to the Apache‑2.0 license terms for the main library code and gratuitously transfer all rights to the changes made/the addition of tests and related infrastructure to the project (maintainers).

If you add third‑party components:

- State the license clearly.
- Provide links to the original repository.
- Confirm license compatibility with the project’s license.

---

### How to Get Started

1. **Fork the repository** and clone it locally.
2. **Create a dedicated branch** for your task (see “Branch Naming”).
3. Make changes while following the code style and test requirements.
4. Run local tests and builds (CMake + compiler).
5. Submit a Pull Request (PR) and fill out the PR template.

### Finding Tasks

- Check Issues with labels: `good-first-issue`, `help-wanted`, `bug`, `enhancement`.
- Look for a curated list of “good first issues” in the README or project board.
- If you want to propose a new feature, open an Issue first to discuss the design.

### Build & Dependencies

#### Requirements

- CMake ≥ 3.20
- Compiler: GCC ≥ 11, Clang ≥ 14, MSVC ≥ 19.30 (VS 2022)
- Python 3.8+ (if build/test scripts are used)
- Git

#### Basic Build (Linux/macOS)

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

#### Build on Windows (MSVC)

```bat
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

#### Build with Tests

```bash
mkdir build && cd build
cmake -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
ctest --output-on-failure
```

### Coding Style & Formatting

- Style guide: follow the project’s established C/C++ style (aligned with libmdbx conventions).
- Use `.c++` and `.h++` for C++ source code files.
- Formatting: use `clang-format` with the provided `.clang-format` config.
- Run before committing:
  ```bash
  git ls-files | grep -E '\.(c|cxx|cc|cpp|h|hxx|hpp|c\+\+|h\+\+)(\.in)?$' | xargs -r clang-format -i --style=file
  ```
- Use spaces (typically 2) instead of tabs.
- Naming:
  - `snake_case` for variables and functions.
  - `PascalCase` for classes/types.
  - `UPPER_SNAKE` for macros.

### Tests & Quality Assurance

- Every new feature or significant change must include tests (GoogleTest, Catch2, or the project’s test framework).
- Tests must be deterministic and must not depend on external services.
- For benchmarks, use a separate target and avoid running them in standard CI test jobs.
- Check for memory leaks and undefined behavior using tools like Valgrind or sanitizers for major changes.

Example with AddressSanitizer:

```bash
make test-asan && make test-ubsan
```
or
```bash
 cmake -DENABLE_ASAN=ON .. && cmake --build . && ctest
 cmake -DENABLE_UBSAN=ON .. && cmake --build . && ctest
 ```

Given libmdbx’s focus on memory-mapped storage and low-level details, pay special attention to:
- Proper handling of memory mapping and file descriptors.
- Correct usage of locking and concurrency primitives.
- Edge cases around page sizes, alignment, and truncation.
- Deterministic behavior across platforms and filesystems.

### Documentation & Examples

- Update `README.md` if the public API changes.
- Add usage examples to the `examples/` directory for new features.
- Document public APIs using the project’s documentation system (e.g., Doxygen) and consistent comment style.

Given libmdbx’s low-level nature, documentation should clarify:
- Threading and concurrency guarantees.
- Transaction semantics and isolation levels.
- B‑tree behavior, page layout, and key/value constraints.
- Platform-specific behaviors and limitations.

### Pull Requests & Commits

#### Branch Naming

Use prefixes to indicate intent:

- `feat/` – new functionality
- `fix/` – bug fixes
- `test/` – tests
- `docs/` – documentation
- `refactor/` – refactoring
- `perf/` – performance improvements

Example: `feat/add-cursor-batch-api`

#### Commit Messages

Follow Conventional Commits:

```text
feat: add batch cursor API

Description: implement a new batch-oriented cursor interface for improved throughput.
Closes #123
```

Format: `<type>: <short summary>` + blank line + detailed description + `Closes #<number>`.

Types: `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`, `ci`, `cmake`.

#### PR Template

In the PR description, include:

- What was done and why
- How to verify (locally or via CI)
- Which tests were added/modified
- Performance/memory impact (if relevant)
- Links to related Issues/discussions

### Review & Merge Process

1. CI must pass (build, tests, linters, coverage).
2. At least 1–2 reviewers must approve the PR.
3. Resolve any conflicts with `main` before merging.
4. Maintainers will perform the merge.

### Communication

- Use the [telegram group](https://t.me/libmdbx).
- Ask questions and discuss ideas via Issues.
- Use Discussions for broader topics.
- Avoid direct messages to maintainers unless previously agreed.

### Common Reasons for Rejecting PRs

- No tests for new functionality.
- Coverage drops without justification.
- Code style violations (not formatted with `clang-format`).
- Non‑atomic commits with unrelated changes.
- Failing CI (build, test, linter, or coverage jobs).
- Missing description and verification steps in the PR.

### libmdbx‑Specific Recommendations

Because libmdbx is a high‑performance, low‑level key‑value store with memory‑mapped files:

- Be explicit about any changes affecting B‑tree structure, page layout, or transaction semantics.
- Include tests that cover edge cases: small/large keys, partial pages, truncation, corruption recovery, and multi‑process scenarios.
- When modifying concurrency or locking, provide reasoning and, if possible, stress tests.
- For any change that affects on‑disk format or compatibility, coordinate with maintainers before submitting a large PR.
- Keep performance implications clear: latency, throughput, memory usage, and any trade‑offs.
