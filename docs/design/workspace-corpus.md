# What tmuxp's own examples say about this surface

The workspace consumer exists to put weight on the library, so the honest
test is somebody else's documents rather than the ones its own tests were
written against. tmuxp ships 23 examples. Run through
`libtmux::workspace::parse_tmuxp`, **twenty-one are read and two refused.**
Nine were read the first time it was run.

That number is not a compatibility score. Full tmuxp runtime compatibility
was never the aim, and most of the refusals are the parser declining to
guess rather than the library being unable to do the thing. What the run is
for is the third category below: keys that no amount of parser work would
help, because the C++ surface had nowhere to put them.

## The library could not express it

`environment:` at session level, and `window_index:`. Both are on the Python
API and neither was on this one. They are now — `environment` as name and
value pairs on all three creation option structs, `index` on
`NewWindowOptions` — and the parser and builder carry them through, which is
what took the count from nine to eleven. The corpus is how they were found.

Finding those cost more than it looked like it would. Adding `environment`
meant passing `-e` on a creation command, which put a second flag after the
`--` that a shell command already needed, which is how it came out that the
format asking for the new entity's fields was going there too — so every
creation call carrying a shell command had been answering with an entity
nobody could read. That bug had no tmuxp document behind it and no test:
the consumer builds through raw argv batches and never parses what creation
returns.

## A command is not a string

That was the largest single thing wrong, and it accounted for four
documents. A tmuxp command is written either as a string or as a mapping
carrying `cmd`, `enter`, `sleep_before` and `sleep_after`, and a
`std::vector<std::string>` cannot hold the second form. It is a
`std::vector<Command>` now.

`enter: false` needed nothing from the library, which already draws that
line: `send_text` never appends a newline and `send_key` submits, so
holding a command back is simply not sending the second one. The pauses are
the builder's own waiting — tmux has nothing to offer there.

## The parser still declines to guess

Two documents, and both ask for something tmux does not do. `before_script`
runs a program before the workspace is built, and `plugins` is tmuxp's own
extension system.

Refusing is deliberate and worth keeping. A dropped `shell_command_before`
builds a workspace whose panes never activate their environment, and
nothing about the result looks wrong; a refusal names the key and the path
to it. The cost is that a document using one key this parser has not learned
is refused whole.

What closed the rest: `global_options` and session `options`,
`options_after`, `window_shell` and a per-pane `shell`, and the pane-level
defaults a document sets once for every command below it. `suppress_history`
turned out to be honourable rather than out of scope — it means sending the
command with a leading space, which is a thing this builder can do.

`options_after` exists because the order matters and tmux will not say so:
`synchronize-panes` set before the splits types into panes that are still
being made, so a window's options are applied in two passes.

## Not the library's to do

`plugins:` is tmuxp's plugin system. `suppress_history:` is a convention
about how a command is sent, not a tmux capability. `before_script:` runs a
program before the workspace is built, which a caller does for itself.

## Running it again

The corpus is not vendored — it lives with tmuxp, and pinning a copy here
would freeze the thing being tested against. Point the parser at whatever
examples are to hand:

```console
$ cmake --build --preset cxx-dev --target tmuxp_corpus_probe
```

```console
$ ./build/cxx-dev/consumers/workspace/tmuxp_corpus_probe ~/src/tmuxp/examples/*.yaml
```
