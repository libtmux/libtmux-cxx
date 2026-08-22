# Writing

How this project writes prose, for humans and agents alike. It governs
`README.md`, `CHANGELOG.md`, release notes, commit messages, CLI and help
text, error messages, API documentation, source comments, and migration
guides.

For build, test, and pull request workflow, see
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## Voice

Every surface has one voice. A doc comment says what a caller may rely on, a
changelog entry says what changed, an error message says what happened. All
three are present tense, lead with the thing being described, and stop. Why it
was written that way belongs in the commit message, which is timestamped and
attached to the diff.

Write for the engineer integrating this library at two in the morning, not for
the one who wrote it. They are judging whether it fits their build.

The most useful editing operation is deleting the first sentence.

| Instead of | Prefer |
| --- | --- |
| "We added…" | "`Server::control` now…" |
| "New and improved" | state what changed |
| "powerful", "seamless" | state the capability |
| "easily", "simply" | omit |
| "robust" | name the failure that is handled |
| "comprehensive" | name what is covered |
| "production-ready" | state the guarantee |
| "optimized", "blazing" | give the magnitude |
| "various fixes" | name the components |
| "under the hood" | omit unless a caller can observe it |
| "please note that" | state the fact |
| "leverage", "utilize" | "use" |
| "in order to" | "to" |

Precision is the whole style, and a C++ reader is grading you on it. Say which
of these you mean:

| Weaker claim | Different, stronger claim |
| --- | --- |
| works | is supported |
| passes our tests | guaranteed by the public contract |
| the compiler accepts it | the standard specifies it |
| does not crash | has defined behaviour |
| faster | faster under these measured conditions |

Claim the weaker one when it is what you have. An honest "works on clang 19,
untested elsewhere" costs nothing; a "supported" that turns out to mean
"nobody tried it" costs a user their afternoon.

## README

The first screen answers what this is, who it is for, which C++ standard,
which compilers, which tmux releases, which platforms, how to build it, and
what a call looks like. A reader is judging integration risk before elegance,
which is why `Requirements & support` sits above the tour and not below it.

One golden path. The preset that works from a clean checkout goes first;
FetchContent, submodules, vcpkg and build options come after it, in that
order, and none of them interrupts the first thing a reader needs.

Examples compile, and the README is not trusted to say so. Every `cpp` block
in it is a named region of `examples/05-readme.cpp`, which builds and runs
against real tmux in the same CTest run as everything else, and
[`tools/docs/check_readme.py`](../tools/docs/check_readme.py) fails the build
when a block and its region drift apart, or when a region nothing quotes goes
stale. Never write a signature that does not exist to make a point read better.

Say what the library is not for. "When you might not need this" is that
section here, and it earns more trust than a feature list: a reader who bounces
off it in ten seconds was never going to be a happy user.

Two facts are fixed and identical across every libtmux port. Do not reword
them, and do not let a port drift:

The title is **libtmux for C++**.

The alpha warning states these terms:

```markdown
> [!WARNING]
> **Alpha.** Releases carry an `-alpha` prerelease tag. The API is not
> settled, and any release may change or remove exported identifiers without a
> deprecation period. Pin an exact version. Not recommended for production.
```

## Changelog

A ledger, not a narrative. It is scanned, and the question a reader arrives
with is whether an entry affects them, so one change gets one bullet.

A release that breaks something opens with `### Breaking`, ahead of every
component section, because that is the entry deciding whether the reader takes
the upgrade at all. Name the compatibility class, and give the way forward as
code rather than as a description of code:

```markdown
### Breaking

- `Pane::capture` returns `std::vector<std::string>` rather than one joined
  `std::string`. Source-breaking; ABI-breaking.

  Before: `auto text = *pane.capture();`
  After:  `auto text = join(*pane.capture());`
```

Everything else groups by the component affected — `Server`, `Session`,
`Window`, `Pane`, control mode, the MCP server, vcpkg, the build — rather than
by whether something is a feature or a fix. A reader knows which part they use
and does not know which category you filed it under.

```markdown
### Control mode

- `Connection::subscribe` now rejects a format string containing a newline
  rather than sending it. tmux terminates the connection on one.
- Add `Connection::drain`, which returns the notifications buffered since the
  last read instead of blocking for the next one.
```

Lead with the identifier and a concrete verb — add, fix, remove, deprecate,
require, `now`, `no longer`. Name identifiers literally: `Pane::send_key`,
`LIBTMUX_USE_TL_EXPECTED`, `-l 25%`. One to three sentences.

State a changed default explicitly, even where nothing breaks, with the way
back in the same bullet:

```markdown
- `show_option` now reads at session scope when the target does not say.
  Pass `scope: window` for the previous behaviour.
```

Do not sell a fix: "no longer reports a truncated answer as a complete one",
not "improves reliability". Do not describe effort. Give the old behaviour
only where it explains a break, and mention mechanism only where a caller can
observe it — a refactor nothing observable comes out of is not an entry.

Link the pull request that carried the change, as a trailing `(#149)`. The
changelog is the only place mapping a behaviour to the diff that caused it, and
a reader chasing detail has nowhere else to start. This is the one surface
where a tracker reference belongs — [Source comments](#source-comments) bans
them in code, because a comment is read without a browser and rots when the
tracker moves, while an entry here is written once and never revisited. A
security entry carries its GHSA or CVE id instead: that is the identifier an
auditor searches for.

Entries land under `## Unreleased`. The maintainer assigns the version when
cutting a release, so nothing written here predicts one.

## Release notes

Impact first, mechanism second. A reader is deciding whether to take the
upgrade, and the compatibility class decides it for them, so that goes near
the top rather than in a footnote.

Performance claims are measurements, and this audience has been burned by
microbenchmarks for thirty years. Give the magnitude and what produced it:

```markdown
Control mode answers a listing in 1.8 ms against the subprocess transport's
8.3 ms — one hundred `windows()` listings against a server holding 21
windows, release build, tmux 3.7b.
```

Not "significantly faster". A number with no method attached is weaker than no
number at all, because it invites a reader to check it.

The release process itself, including the rule that nobody but the maintainer
creates a tag, is in [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Commit messages

Write for someone reading `git log` in a year with no memory of the
discussion. The diff already shows how; the message says what changed and what
made it necessary.

```
Scope(type[detail]): concise description

why: Explanation of necessity or impact.

what:
- Specific technical changes made
- Focused on a single topic
```

Keep the subject to 50 characters or fewer, excluding any trailing `(#NN)` pull
request reference, and wrap body lines at 72. Separate `why:` and `what:` with
a blank line.

Types in use: **feat**, **fix**, **refactor**, **docs**, **chore**, **test**,
**style**, plus `cxx(deps)` for dependencies, `cxx(deps[dev])` for development
dependencies, and `ai(rules[AGENTS])` for agent rule changes.

```
Pane(feat[send_keys]): Add support for a literal flag

why: Send characters without tmux interpreting them.

what:
- Add a literal field to send_keys_options
- Pass -l when it is set
```

One semantic change per commit. A rename that changes no behaviour is its own
commit, and so is a reformat: a diff that mixes them with a behavioural change
cannot be reviewed, and cannot be reverted without taking the rest with it.

Every commit builds and passes on its own. `git bisect` is a thing this project
ships rather than hopes for, and a commit that only compiles once the next one
lands makes a bisect report a lie.

Name the alternative you rejected, in a line or two, when there was one. It is
the most valuable thing a commit message carries and the first thing lost: the
next person to look at this code — or the next agent asked to tidy it — will
reach for the same discarded approach, and the message is what stops them.

```
why: The tmpfile round trip existed because control-mode chunking was
unreliable before tmux 3.3, which the supported range no longer includes.

Rejected keeping it behind a runtime probe: two paths for one feature, to
serve a release nothing in the matrix builds against.
```

Cite a standard or a defect by a stable link rather than by description —
`wg21.link/p2900`, a GCC or LLVM bug number, a tmux issue. A reader who cannot
follow the reference cannot check the reasoning, and a paraphrase decays into
folklore.

Release commit subjects are plain and short — `Tag v<version>` — with the
detail in the body. Do not bury the lede under a scope prefix.

Use a heredoc so the formatting survives:

```console
$ git commit -m "$(cat <<'EOF'
Scope(feat[detail]): Concise description

why: Explanation of the change.

what:
- First change
- Second change
EOF
)"
```

## Command-line help

Help text is read in a terminal by someone who is already stuck, so it answers
in one screen and does not send them to the README.

Each flag says what it does, what happens without it, and what it implies.
A default is stated rather than left to be discovered:

```text
--prefix DIR    Install prefix to take a published server from; its bin/ and
                tools/libtmux/ are searched, covering a cmake --install and a
                vcpkg install. Implies --source published.
```

Name the real paths, the real environment variables and the real defaults.
"the usual place" is not help.

Document the exit codes wherever a program has more than success and failure.
A caller writing a shell script around a tool needs them, and guessing from a
run that happened to work is how a script acquires a bug that fires once a
year.

## Error messages

An error says what happened, in a phrase that completes a sentence the caller
is already writing. Lowercase, no trailing period, no blame:

```cpp
case FailureKind::validation:
  return "the request was rejected before tmux ran";
case FailureKind::timeout:
  return "tmux did not answer in time";
```

Name the actor. "tmux refused the command" and "tmux could not be started"
tell a caller which side of the boundary broke, which is the first thing they
need and the thing a generic "operation failed" withholds.

Where a diagnostic quotes what a program actually printed, quote it exactly.
An error text is a search key, and a paraphrase is not findable.

Do not apologise, do not suggest what the caller probably meant unless the
answer is certain, and never end with an exclamation mark.

## API documentation

The header is the manual. `include/libtmux/` is the contract, so the
documentation lives on the declaration there — not in `src/`, and not
duplicated in both.

The type system documents first. A concept, `[[nodiscard]]`, `noexcept`,
`explicit`, and a parameter type that cannot be passed the wrong thing state a
contract the compiler enforces, which no comment can claim. Prose covers what
is left over. C++26 contracts are out of reach at this floor, so a
precondition the type cannot carry stays a sentence.

Write plain `//` prose above the declaration. No Doxygen tags:
`tools/docs/api_index.py` harvests the comment block above each declaration
into [`docs/api.md`](../docs/api.md) as prose, and an `@param` line arrives at
the reader as the characters it is.

Do not restate the signature. This has negative value:

```cpp
// Returns the size.
std::size_t size() const;
```

Document what the reader cannot see:

```cpp
// The number of panes in the snapshot this window came from.
//
// Reads no tmux: the listing ran once, when the snapshot was taken, so the
// count is that moment's and does not follow a pane created since.
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
  the backend will not do it at all. `unsupported` is the one worth spelling
  out: it is not a failure the caller can retry or fix, it is the surface
  saying this backend does not carry that operation.
- **Exceptions.** The library does not throw to report a tmux failure.
  Anything that can still throw — allocation, a caller's own observer — says
  so.
- **Preconditions and postconditions** that the type cannot express.
- **Complexity and allocation**, where a caller would otherwise have to guess.
- **ABI**, for anything whose layout is part of the installed surface.

The reference is generated, and CI fails when it drifts from the headers:

```console
$ python3 tools/docs/api_index.py \
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
maintain.

A comment often holds both a constraint and the deliberation that found it.
Keep the constraint, cut the deliberation. "Runs at most once per second"
survives; "this is the right trade for now" does not.

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

The exception to the rule above about references. Code that is strange because
the standard, an ABI, or a compiler made it strange says which one, by name:

```cpp
// tmux 3.4 removed split-window's -p, which 3.3a and 3.5 both take. Spell the
// percentage as -l 25% instead; every supported release accepts that.
```

A version, a paper number, a CWG issue, a named compiler defect, or a section
of an ABI document is what stops a later cleanup from deleting the weird thing
and reintroducing the bug. Those citations are frozen external facts, so the
upkeep gate does not reach them.

### The upkeep gate in practice

It reaches values that track our own code. It does not reach frozen external
facts.

Bad (Delete):

```cpp
// There are 321 tests to complete for servers.
```

Good (Keep):

```cpp
// tmux < 3.2 reports the pane ID only after the command completes,
// so this query must stay separate.
```

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

A change is usually not one of these. "Source-breaking; ABI-compatible" and
"no source change required, but callers must relink" are complete statements.
"Breaking" on its own is not.

This project carries no `SOVERSION`. The inline namespace pair is the whole
ABI statement, so an ABI break is described by what it does to those namespaces
and to a caller who must relink — not by a soname that does not exist.

Name toolchains and versions exactly. clang, GCC, MSVC, libc++, libstdc++ and
Apple Clang are not interchangeable, and neither are tmux releases: `3.7a` is
not `3.7`, and `master` is not a version. Write `clang 17+`, `tmux 3.2a`,
`CMake 3.25` — never "a recent compiler". tmux and psmux are different
programs, and a sentence true of one is not automatically true of the other.

Say which platform a claim holds on. Since the Windows preview landed, an
unqualified "supported" reads as all of them, and most of this surface is
POSIX-only.

While the project is alpha, none of these are promised. Say that plainly where
a reader would otherwise assume otherwise, rather than implying stability by
staying quiet.

## Terminology and capitalization

`libtmux`, `tmux` and `tmuxp` are lowercase, including at the start of a
sentence. This project is **libtmux for C++**; the Python original is
**libtmux**, and it is the reference the parity ledger measures against.

Headings are sentence case: "Build from source", not "Build From Source".

Identifiers go in backticks, spelled exactly as the code spells them —
`Server::from_env()`, `FailureKind::refused`, `LIBTMUX_CXX_STANDARD`,
`--no-tests=error`. An identifier paraphrased into prose stops being
searchable, and search is how both readers find it.

Quote the diagnostic a reader will actually see, rather than describing it:
"fails with `File name too long`" beats "may fail with a path error". Exact
strings are search keys.

Spelling is British — behaviour, canonicalise, serialise — matching the prose
already here. No emoji in commits, issues, pull request comments, or code.

## Markdown

Markdown files in this repository wrap at 80 columns. Pull request and issue
bodies do not: GitHub renders a single newline as a space in a file and as a
line break in a comment, so a wrapped comment body arrives as ragged stubs.

A URL and a command that will not split stay on one line and run long. Breaking
either to satisfy the margin breaks the thing itself.

GitHub alert blocks — `> [!WARNING]`, `> [!NOTE]` — are allowed here, unlike
in some sibling ports. This project's readers arrive through GitHub and
through the vcpkg registry, both of which render them, and the README's alpha
warning is one. Do not "fix" them into plain paragraphs.

Links are relative and point at files in the repository. Never a local
absolute path.

## Code blocks

Code blocks are paste-and-run units: pasting one block runs exactly one
intended action. Executed examples are exempt — the suite runs them, nobody
pastes them.

- **One command per block.** Multiple steps may share a block only when
  explicitly chained with `&&`, `;`, or `\` continuations — the chain is then
  one logical command.
- **Explanations go in prose above the block**, never as `#` comments inside
  it.
- **Command menus are per-command blocks with prose lead-ins**, not tables.
- **Shell commands use the `console` tag with a `$ ` prefix.** This separates
  interactive commands from scripts and enables prompt-aware copy.
- **Split long commands with `\`** — one flag or flag-and-value pair per
  indented continuation line, positional arguments last.

Good:

Show the last ten commits as a graph:

```console
$ git log \
    --max-count=10 \
    --graph \
    --oneline
```

Bad:

```console
# Show the last ten commits as a graph
$ git log --max-count=10 --graph --oneline
```

## Slop prevention

Treat AI slop as review-hostile noise, not as proof that the text or code is
wrong. The goal is to maximise information density.

- **AI signatures.** No "Generated by", no conversational filler, no
  unexplained emoji, no tool metadata. No `Co-Authored-By` naming a tool and no
  trailer recording that a model wrote something: git already records
  authorship, and a commit is judged on what it says.
- **Brittle references.** No hard-coded line numbers, fragile file counts,
  dated "as of" claims, bare SHAs, or local absolute paths — unless they are
  strict evidentiary artefacts such as a benchmark log.
- **Diff narration.** Do not restate what moved, was renamed, or was removed
  in anything the reader holds alongside the diff: code, doc comments, README,
  or a pull request description. The diff and the commit message carry it.
- **Branch-internal narrative.** Do not mention intermediate states, abandoned
  approaches, or "no longer" behaviour unless users of a published release
  actually experienced the old state.
- **Low-value scaffolding.** No ownerless TODOs, unused future-proofing, debug
  artefacts, or defensive wrappers around failure modes nothing can reach.
- **Prose inflation.** The table under [Voice](#voice) governs.
- **Coded labels.** Write rules and findings as plain imperatives. No `[R1]`,
  `Option B`, or any index a reader has to decode.

Preserve the "why". Never delete a comment documenting an invariant, a
protocol constraint, a platform quirk, or an upstream workaround — those are
the facts [Source comments](#source-comments) keeps, and every other comment
is judged by it.
