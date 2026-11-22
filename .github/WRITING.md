# Writing

This guide extends the organization writing guide at
[`libtmux/.github` `WRITING.md`](https://github.com/libtmux/.github/blob/main/WRITING.md)
with the rules specific to this repository: C++ claims, `README.md`,
changelogs, Doxygen comments, source comments, compatibility vocabulary, and
terminology.

For build, test, and pull request workflow, see
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## Voice

Every surface states what a caller may rely on: doc comments state contracts,
changelog entries state observable changes, and error messages state what
happened. Rationale belongs in the commit message alongside the diff.

Say which claim holds, and claim the weaker one when that is what was tested:

| Weaker claim | Stronger claim |
| --- | --- |
| works | is supported |
| passes our tests | guaranteed by the public contract |
| the compiler accepts it | the standard specifies it |
| does not crash | has defined behaviour |
| faster | faster under these measured conditions |

## README

The title is **libtmux for C++**, and the alpha notice is a plain CommonMark
blockquote:

```markdown
> **Alpha.** Releases carry an `-alpha` prerelease tag. The API is not
> settled, and any release may change or remove exported identifiers without a
> deprecation period. Pin an exact version. Not recommended for production.
```

The root `README.md` and each package README follow one scannable shape:

- **Header.** The shared logo block, a centred title, and badges that answer
  200: CI, docs on `libtmux.org`, licence, and C++ standard.
- **Quick start and installation first.** Open with a runnable quickstart and
  the installation paths (`FetchContent`, source install, submodule, `vcpkg`,
  and build options) plus a concise floor statement.
- **Tasks and APIs.** Each `##` heading names a task in the reader's words.
  Each `###` heading links the API on `libtmux.org`, followed by a colon and
  its job in plain words:

  ```markdown
  ### [`Server::run_chain`](https://libtmux.org/en/cxx/latest/reference/libtmux-server-run_chain/): several commands, one process
  ```

- **Under a heading.** Link sibling APIs on first mention, show the compiled
  example and what it prints, keep prose to three lines or fewer, and link
  deeper guides with `More:`.
- **Compiled examples only.** `cpp` blocks quote named `#region` blocks in
  [`examples/05-readme.cpp`](../examples/05-readme.cpp) or the whole
  [`examples/08-quickstart.cpp`](../examples/08-quickstart.cpp). Both build and
  run against real tmux, and
  [`scripts/docs/check_readme.py`](../scripts/docs/check_readme.py) fails when a
  block drifts or a region goes unquoted. Examples start with `#include` lines
  (or continue from the section's opening include block) and own their private
  tmux server through the library.
- **Headline features.** Show typed filters, `Chain` and `Server::run_chain`,
  and `Server::control` and `Server::over_control`. This port has no planner.
- **Packages, exclusions, and support.** List the packages with trunk links and
  reference/changelog links, state what the library leaves out, and keep
  version and platform floors under `Support and status`.

## Changelog

Each package keeps its own `CHANGELOG.md`: `apps/mcp/CHANGELOG.md` for
`libtmux-mcp-server`, `apps/workspace/CHANGELOG.md` for `tmux-workspace`, and
the root `CHANGELOG.md` for `libtmux` and cross-package changes. The packages
share one version, so every file carries every release heading; a release with
no package changes states ``No entries for `tmux-workspace`.`` under it.

Entries land under `## Unreleased`, one bullet per observable change, trailing
the pull request as `(#149)` (or a GHSA/CVE id for security fixes). Group by
component (`Server`, `Session`, `Window`, `Pane`, control mode, the MCP server,
vcpkg, the build) rather than feature versus fix. Put `### Breaking` first in
the broken package's file, naming the compatibility class and showing `Before:`
and `After:` as code:

```markdown
### Breaking

- `Pane::capture` returns `std::vector<std::string>` rather than one joined
  `std::string`. Source-breaking; ABI-breaking.

  Before: `auto text = *pane.capture();`
  After:  `auto text = join(*pane.capture());`
```

Lead with the identifier and a concrete verb (`add`, `fix`, `remove`,
`deprecate`, `require`, `now`, `no longer`), state changed defaults with the
way back, and omit unobservable refactors.

## Release notes

Put the compatibility class near the top so a reader can judge the upgrade at
once. Attach every performance number to its method and environment:

```markdown
Control mode answers a listing in 1.8 ms against the subprocess transport's
8.3 ms — one hundred `windows()` listings against a server holding 21
windows, release build, tmux 3.7b.
```

## Commit messages

Follow the `Scope(type[detail]): concise description` format, 50/72 limits, and
`why:` / `what:` body in the
[organization writing guide](https://github.com/libtmux/.github/blob/main/WRITING.md#commits-and-pull-requests).

- **Types in use.** `feat`, `fix`, `refactor`, `docs`, `chore`, `test`,
  `style`, plus `cxx(deps)` for dependencies, `cxx(deps[dev])` for development
  dependencies, `ai(rules[AGENTS])` for agent rules, and plain `Tag v<version>`
  for release commits.
- **Bisectable commits.** One semantic change per commit; every commit builds
  and passes on its own.
- **Rejected alternatives and citations.** Record discarded approaches in a
  line or two under `why:`, and cite standards or defects by stable identifiers
  (`wg21.link/p2900`, GCC/LLVM bug numbers, tmux issues).

## Command-line help and error messages

Help text fits one screen and states what each flag does, what happens without
it, real paths and environment variables, defaults, and exit codes:

```text
--prefix DIR    Install prefix to take a published server from; its bin/ and
                tools/libtmux/ are searched, covering a cmake --install and a
                vcpkg install. Implies --source published.
```

Error messages are lowercase phrases with no trailing period, naming the actor
(`tmux` versus the request) and quoting exact diagnostics so they remain
searchable:

```cpp
case FailureKind::validation:
  return "the request was rejected before tmux ran";
case FailureKind::timeout:
  return "tmux did not answer in time";
```

## API documentation

The header is the manual. `include/libtmux/` is the contract, so the
documentation lives on the declaration there — not in `src/`, and not
duplicated in both.

The type system documents first. A concept, `[[nodiscard]]`, `noexcept`,
`explicit`, and a parameter type that cannot be passed the wrong thing state a
contract the compiler enforces, which no comment can claim. Prose covers what
is left over. C++26 contracts are out of reach at this floor, so a
precondition the type cannot carry stays a sentence.

Write the prose above the declaration as `///`. Two generators read it and
only one of them reads a plain `//`: Doxygen, whose XML is what `libtmux.org`
renders, treats `//` as an ordinary source comment and drops the block
entirely. A file-level block, which documents a file rather than the next
declaration, stays `//`.

No Doxygen tags. Both generators render the block as prose, so an `@param`
line arrives at the reader as the characters it is.

Doxygen also parses the Markdown, which is worth two habits: backtick
anything that would otherwise read as a tag or a command — `<ostream>`,
`\t` — and write a span containing a lone `'` with double backticks, since a
`'` outside a word swallows the text after it. CI fails the build on either.

Do not restate the signature. Document what the reader cannot see:

```cpp
/// The number of panes in the snapshot this window came from.
///
/// Reads no tmux: the listing ran once, when the snapshot was taken, so the
/// count is that moment's and does not follow a pane created since.
[[nodiscard]] std::size_t pane_count() const noexcept;
```

The subjects worth the space, roughly in the order they bite:

- **Lifetime and ownership.** What a returned reference borrows from, and what
  invalidates it.
- **Snapshot semantics.** Which call reached tmux, and when. An entity that
  reads its own fields without a process is the design, and a caller who
  misses that will chase a stale value.
- **Thread safety.** What may be called concurrently, and on which thread a
  callback runs. `CommandObserver` runs on the thread that ran the command,
  with nothing held.
- **Failure.** Failure is a value here, so say which `FailureKind` a caller
  should expect when it is not obvious from the call — `validation` before
  tmux ran, `refused` when tmux ran and said no, `timeout` when it never
  answered, `truncated` when the answer did not fit, and `unsupported` when
  the backend will not do it at all.
- **Exceptions.** The library does not throw to report a tmux failure.
  Anything that can still throw — allocation, a caller's own observer — says
  so.
- **Preconditions and postconditions** that the type cannot express.
- **Complexity and allocation**, where a caller would otherwise have to guess.
- **ABI**, for anything whose layout is part of the installed surface.

The reference is generated, and CI fails when it drifts from the headers:

```console
$ uv run scripts/docs/api_index.py \
    --include include/libtmux \
    --output docs/api.md
```

## Source comments

A comment ships only if it passes all three gates. Fail any: delete or
rewrite. Borderline: delete — borderline means the information is
reconstructible, which is what makes deletion cheap.

**Loss.** Three years from now, would losing this cost a maintainer real time
rediscovering intent, an invariant, a constraint, or a failure mode the code
and tests do not already make obvious?

**Elite.** Would SQLite, Redis, the Go standard library, or CPython write this
comment, at this length? Those projects state the constraint and stop. They do
not argue with an imagined objector.

**Upkeep.** Will it stay true without maintenance? A comment that hand-syncs a
value the code owns — a count, an offset, a line reference, a duplicated
constant — is false the first time that value moves.

### Ceiling

One or two lines. A comment reaching four is either carrying several facts, in
which case split it, or arguing, in which case cut it to the fact.

Rationale, alternatives weighed, and the story of how the code got here belong
in the commit message: timestamped, attached to the exact diff, and free to
maintain. Keep the constraint, cut the deliberation.

### Keep

- Why over how: upstream quirks, protocol and compatibility constraints,
  performance tradeoffs still part of the contract.
- Invariants, preconditions, ordering, lifetime, and concurrency requirements
  that types and tests cannot express.
- Code that looks wrong but is not, so a later cleanup does not reintroduce
  the bug.
- A high-level sketch of an algorithm whose local operations do not reveal the
  whole.

### Delete

- Narration of the next lines; code translated into English.
- Restated names, types, defaults, or control flow.
- Values duplicated from the code and hand-synced.
- Justification, hedging, or apology for a choice.
- Speculation about future requirements.
- History version control already holds, including commented-out code.
- Ticket and issue numbers. They say nothing to a reader without tracker
  access, and they rot when the tracker moves. Unfinished work goes in the
  tracker, not the source.
- Transient observations — "currently", "for now", "the latest release" —
  that go stale with no nearby edit.

### Cite the authority

Code that is strange because the standard, an ABI, or a compiler made it
strange says which one, by name:

```cpp
// tmux 3.4 removed `split-window -p`, so the percentage is spelled
// `-l 25%`, which every supported release accepts.
```

A version, a paper number, a CWG issue, a named compiler defect, or a section
of an ABI document stops a later cleanup from deleting the workaround and
reintroducing the bug. Those citations are frozen external facts, so the upkeep
gate does not reach them; hand-synced counts (`// 321 server tests`) fail it.

### Documentation exception

Minimal usage examples, and the lines describing a parameter, a return value
or a failure on public API, are exempt from the loss gate — they serve the
caller, not the maintainer. They are exempt from nothing else. Ceiling: a good
man page entry.

Public header comments harvested into [`docs/api.md`](../docs/api.md) fall
under this exception, as do the programs in `examples/`, which the suite runs.

## Compatibility vocabulary

Four different claims wear the word "compatible". Say which one is meant:

- **Source compatibility.** Existing code still compiles.
- **Binary and ABI compatibility.** Existing objects still link, and still
  read the right bytes. The C++20 and C++23 builds are not ABI-compatible with
  each other, which is why each lives in its own inline namespace —
  `v1_cxx20` and `v1_cxx23` — so mixing them is a link error naming the
  missing symbol rather than a program that reads the wrong offsets.
- **Behavioural compatibility.** The same call still does the same thing.
- **Wire compatibility.** What crosses a control connection, or what a tmux
  release will accept, still parses.

"Source-breaking; ABI-compatible" and "no source change required, but callers
must relink" are complete statements; "breaking" alone is not. This project
carries no `SOVERSION`: describe ABI breaks by their effect on `v1_cxx20` and
`v1_cxx23` and on callers who must relink.

Name toolchains, releases, and platforms explicitly (`clang 17+`, `tmux 3.3a`,
`CMake 3.25`; tmux versus psmux; Linux versus macOS or the Windows preview).
While the project is alpha, none of these compatibility classes are promised.

## Terminology and capitalization

`libtmux`, `tmux` and `tmuxp` are lowercase, including at the start of a
sentence. This project is **libtmux for C++**; the Python original is
**libtmux**.

Headings are sentence case ("Build from source", not "Build From Source"). Put
identifiers in backticks spelled as the code spells them (`Server::from_env()`,
`FailureKind::refused`, `LIBTMUX_CXX_STANDARD`, `--no-tests=error`), and quote
exact diagnostic strings (`File name too long`). Spelling in prose is British
(`behaviour`, `canonicalise`, `serialise`). No emoji anywhere.

## Markdown and code blocks

Follow the CommonMark and 80-column wrapping rules in the
[organization writing guide](https://github.com/libtmux/.github/blob/main/WRITING.md#markdown):

- Plain CommonMark only; do not use GitHub-only alert blocks (`[!NOTE]`,
  `[!WARNING]`).
- Use relative links to files in this repository, never local absolute paths.
- Keep one logical command per pasted block, prose explanations above the
  block rather than `#` comments inside it, and `console` fences with a `$ `
  prompt and one flag per `\` continuation line.
- Never delete comments that record invariants, protocol constraints, platform
  quirks, or upstream workarounds kept by [Source comments](#source-comments).
