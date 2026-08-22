# Streaming pane output

What a control connection may subscribe to, and what tmux does when the
reader falls behind.

`ConnectionOptions::pane_output` is off, which is what the connection always
did — it passed `-f no-output` and could not be told otherwise. Turning it on
is a decision with consequences that are not obvious from tmux's manual, so
they were measured against tmux 3.7b before the option existed.

## What was measured

A private server, a control client per case, and a pane told to print
thousands of lines.

| Question | Answer |
|---|---|
| Can `no-output` be undone for one pane? | **No.** `refresh-client -A "%0:on"` on a client started with `-f no-output` produced 0 `%output` lines, against 24 for the same actions without the flag. |
| Can one pane be muted on a listening client? | **Yes.** `refresh-client -A "%1:off"` took `%1` from 20 lines to 1 while `%0` stayed at 20. |
| What does `pause-after` do to a slow reader? | **Discards the pane's whole queue.** With `pause-after=1` and a three-second stall: 0 `%output`, one `%pause %0`. Without it, the same stall delivered 280 lines and no pause. |
| Does resuming recover the gap? | **No.** `refresh-client -A "%0:continue"` resumes the stream; the offsets snap to the current end, so what was queued is gone. |
| What does listening cost the command path? | **Almost nothing.** Reply latency under a 20,000-line flood: median 0.2 ms with `no-output`, 0.3 ms with output on; worst case 0.5 ms against 7.4 ms. |

## What follows from it

**Output is a connection-construction choice, not a subscription.** The first
measurement settles it: a connection that starts silent cannot be made to
listen, so there is no per-pane opt-in to offer and pretending otherwise would
be an API that fails at runtime. Muting a pane on a listening connection is
the operation tmux actually supports.

**It stays off by default.** Not for cost — the last measurement shows there
is barely any — but because a caller who has not asked to read pane output
should not have tmux buffering it for them on the server.

**`pause_after` is a data-loss policy, and is named as one.** It is not
backpressure. Below the threshold tmux buffers; at it, the pane's queue is
discarded and `%pause` is written. Leaving it unset is also a policy: tmux
buffers server-side until a queued block is five minutes old and then closes
the connection with `too far behind`. A caller picks which failure they
prefer, and neither is silent — one arrives as `%pause`, the other as the
stream ending.

**A caller who sets `pause_after` must watch for `%pause`.** It is the only
signal that output was dropped, and it names the pane. Nothing else reports
the gap.

## Delivered surface

`Server::control_with_options(session, options)` opens the raw stream without
making the caller rebuild the Server's socket route.
`Server::over_control_with_options(session, options)` applies the same policy
to the entity-oriented surface. In both
forms the Server overrides `socket_path`, the argument overrides
`session_name`, and the caller's executable, deadlines, limits,
`pane_output`, and `pause_after` are preserved.

`Connection::set_pane_output` mutes or resumes one pane, and
`parse(Notification)` turns every known notification into typed borrowed
fields while preserving unknown additions. `NotificationRange` drains those
events to a deadline, and `notification_fd()` lets an external POSIX event loop
wake without polling.

## Related

- [`core/behaviors/flow-control.md`](https://github.com/tmux/tmux) in the tmux
  notes — the pause model's constants and where they live in tmux's source
- [`control-transport.md`](control-transport.md) — why entities dispatch over
  one held-open connection
