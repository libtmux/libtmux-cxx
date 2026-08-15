# Adversarial pass over the buffer, format, scripting and key work

A line-by-line read of the surface added after the buffer file operations,
against the standing brief: needless abstractions, allocations, templates,
dependencies, weak names, unsafe lifetimes, compile-time bloat,
generated-looking code. Three findings, one of them the kind only a
mutation run finds.

## A template that carried two string views

`detail::expand_format` was a template on the entity type, and the entity
contributed exactly two things: the name of its id format field, and the
noun for the diagnostic. Both are `string_view` constants. Three
instantiations of a thirty-line body to obtain them.

The body is a plain function now, taking those two as arguments, with a thin
template forwarding to it so a call site still cannot pair one entity's id
field with another's noun. One body, three callers.

## A guard that repeated what tmux already says

`bind_key` and `unbind_key` refused an empty key before dispatch. The header
directly above them says key names are tmux's to check, because unlike
`send-keys` it reports one it does not know — and a spike confirms that
covers the empty case: `unknown key:` at a non-zero status, with nothing
created, on both commands.

So the guard contradicted its own stated rationale and bought a round trip.
Removed. The table checks stay, and for the opposite reason: tmux *accepts*
a table name containing whitespace and then prints it unquoted, so nothing
downstream can read the listing back.

The test that covered it was weak in a way worth naming. It asserted only
that an empty key failed — which stayed true with the guard disabled,
because tmux refuses it anyway. Mutating the guard away was what exposed
that. It now asserts the failure came from tmux, `dispatched` and all, which
is a claim the guard's absence cannot satisfy and its presence cannot
either.

## What was left alone

`Session`, `Window` and `Pane` each spell `show_message` as a one-line
delegation to `run`. Folding the three into a helper would need the target
passed in, saving no lines and adding a hop, and the same shape is already
how `select`, `kill` and `rename` are written. Repetition that matches the
house idiom is not duplication worth removing.

`append_environment` builds `name + "=" + value`, which allocates twice per
variable. It runs a handful of times when a session is created. Rewriting it
to append in place would trade legibility for nothing measurable.

## Making the pass repeatable

Everything above came from mutation rather than from reading, and mutation
run by hand has a failure mode worse than not running it: a pattern that no
longer matches, or an edit that does not compile, prints nothing and reads
exactly like a suite holding firm. Five of the roughly thirty runs behind
this note were non-results for that reason, and each was only noticed
because the output looked too clean.

So the catalogue lives in `cxx/tools/mutate/` and the runner names three
outcomes instead of two:

```console
$ python3 -m cxx.tools.mutate --preset cxx-dev
```

A survivor fails the run and says which guard went unnoticed. A mutation
that did not apply or did not build fails it too, as a stale catalogue entry
claiming a pass it never earned. Only a kill is a pass.

The entries are the guards that exist because tmux does something quiet:
a target it cannot resolve answered with a blank, a `-e` with no `=` taken
and ignored, a table name printed unquoted. The ordinary test suite also
checks that every pattern still matches its source, so a reworded guard
surfaces without waiting for a mutation run.
