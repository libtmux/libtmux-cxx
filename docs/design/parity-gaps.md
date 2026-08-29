# What the port does not do yet

The parity ledger counts entries. This counts capabilities: every public
callable on `Server`, `Session`, `Window`, `Pane` and `Client` in the Python
library that no C++ name answers.

```console
$ python3 -m tools.parity gaps
```

It prints a total in the mid twenties and the names behind it, grouped by
the class that owns them. They are not evenly spread: they cluster into
whole areas the port has not entered.

The other half of the question — how much of the surface those are — is a
second command:

```console
$ python3 -m tools.parity coverage
```

The ledger's own total is not the denominator. It holds 966 entries, of
which a fifth are `libtmux.neo`, which this port deliberately does not
mirror, and a twentieth is a vendored copy of `packaging`. Read against
that, a pending count says the port has barely started while the hierarchy
it exists to wrap is nearly covered. So the buckets are named and counted,
and the part that matters — the public callables of `Server`, `Session`,
`Window`, `Pane` and `Client` — is reported twice: once for how many some
C++ name answers, and once for how many the ledger records with evidence.
The distance between those two is the real debt, and reporting either alone
hides it.

## Renames, and why the survey is a program

A rename is not a gap. `list_windows` is `windows()`, `capture_pane` is
`capture()`, `send_keys` is `send_text()`, `raise_if_dead` is
`check_alive()`. A plain name diff calls all of those missing, and the
first version of this note — written by hand against such a diff — did,
overstating the work by roughly half.

So the renames are written down, as `RENAMES` in `tools/parity/gaps.py`,
and the survey is generated. The table itself is the hazard: an entry whose
target does not exist would still suppress its Python name, so the survey
would shrink and nothing would say why. `find_gaps` therefore checks every
target against the headers first and raises rather than report anything if
one is absent.

That leaves the bias in one direction only. A rename nobody has recorded
still reads as a gap, so the number is an upper bound — but a *wrong* rename
can no longer hide a real one. When a listed capability turns out to exist,
the fix is a line in the table, which is also what stops the mistake from
coming back.

Ownership is deliberately not part of the match. `Server.delete_buffer` is
answered by `Buffer::remove`, and `Server.show_buffer` by
`Buffer::contents`, because a buffer here is a thing rather than a string
the server hands out. That is a reshaped surface, not a missing capability,
and the survey treats it as covered.

## Interactive modes and overlays

`choose_buffer`, `choose_client`, `choose_tree`, `clock_mode`,
`customize_mode`, `display_panes`, `display_popup` on a pane;
`display_menu`, `command_prompt`, `confirm_before` on the server.

Everything that puts a client into a mode a person then drives. `copy_mode`
is done — `Pane::enter_copy_mode` and `Pane::leave_mode`, the latter
reporting tmux's refusal when the pane is in no mode rather than pretending
it is a no-op. What is left needs an attached client to be worth anything,
which is the reason none of it has been built: the test would have nobody
to show the popup to.

`display_popup` is the exception worth having anyway, since a program that
knows a client is attached can use it to say something.

## Key bindings

`list_keys`.

Binding and unbinding are done — `Server::bind_key` and `Server::unbind_key`,
taking the command as argv so nothing has to be quoted. Reading the bindings
back is the part that is not, and a spike says why.

**A listing cannot be parsed soundly.** tmux accepts a key table whose name
contains a space and then prints it unquoted, so a line reading

```
bind-key    -T my table     X                         display-message x
```

cannot be told apart from the table `my` bound to the key `table` running
`X display-message x`. Nothing in the output disambiguates it. `bind_key`
therefore refuses to create such a name — the same stance targets take
against a session name holding a `:` — but a table someone else created
would still land in the listing, and no parser can be right about it.

There is no format surface to fall back on. `key_table` is the only format
variable tmux exposes; there is no `#{key}` and no `#{command}`, so `-F`
cannot produce the structured listing it produces for sessions, panes,
buffers and commands. The remaining shape of the text is column-aligned to
widths computed per listing, keys are backslash-escaped (`\"`, `\#`,
`C-\\`), and commands come back in tmux's own re-quoted form, braces and
`\;` separators included.

**And `-T` cannot narrow it.** `list-keys -T <table>` returns nothing at all
for a user-created table on 3.7, 3.7a and 3.7b, while still working for
built-in tables and while the unfiltered listing shows the binding. So even
a per-table read has to list everything and filter.

What a `Key` entity needs, then, is a fifth parser over text tmux formats
for people, in the same class as the four already fuzzed — and it should get
the same treatment rather than a hand-rolled split.

`unbind-key` in a table that does not exist is refused on every supported
version, which is what makes a binding observable without reading the
listing at all: bind into a fresh table, and the unbind that then succeeds
would have failed a moment earlier.

## Client control

`detach_all_clients`, `lock_client`, `lock_server`, `suspend_client`,
`switch_client`, `server_access`, `refresh_client` on the server;
`lock_session` and `switch_client` on a session.

`Client::switch_to`, `Client::detach` and `Session::detach_clients` cover
the per-client half. What is missing is the server-wide form, and locking.

Attaching is not on the list. `Session::attach_command` hands back an argv and
retains its server route while the value lives. The caller spawns it because a
library whose commands use pipes does not own the terminal an attached client
needs.

## Scripting the server

`if_shell`, `show_messages`, `show_prompt_history`, `clear_prompt_history`.

What is left is tmux's own record of what it has said, and the conditional
form. `run_shell` and `source_file` are done, as is `wait_for` — which
answers when the server dies underneath the wait rather than reporting the
silence as success.

Two things a spike settled about these. `run-shell` hands back what the
command printed on every supported version except 3.3a and 3.4, which
discard it, so `Server::run_shell` reports whether the command ran rather
than what it said — an answer that is empty on two versions in the middle
of the range is worse than no answer. The exit status, unlike the output,
is uniform, and arrives as `exit_code`.

`show_messages` has the same shape of problem and no way around it: the log
it prints is one line on 3.2a and 3.4 and holds every command run on 3.6
and later, so the same call answers differently depending on the server. It
is the reason that one has not been built.

## Topology

`find_window` and `enter`, both on a pane.

The rest of this area is done. Panes break out and join back, pipe and stop
piping, respawn, take titles and swap; windows link, unlink, name their
linked sessions, move, rotate and cycle layouts. `enter` is a one-key
convenience over `send_key`; `find_window` is a search, and the query
surface is where searching lives here.

## What this list is for

It is the difference between "the port works" and "the port is at parity",
stated as work rather than as a percentage. A reader deciding whether the
library can do their job wants capabilities, not entry counts.
