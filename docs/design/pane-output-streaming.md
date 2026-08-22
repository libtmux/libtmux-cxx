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
| What does the way a caller takes the stream cost? | **Everything, if it polls.** Over one three-second window on the same workload, a `take_notifications` loop that sleeps a millisecond between empty takes woke a median 2561 times; `wait_for_notifications` woke 18 and `notification_fd` under `poll` woke 22, for the same notifications. |

The consumption figures are from this repository's own surface rather than raw
tmux: one connection opened with `pane_output` and `pause_after`, 200
`send-keys` echoes dispatched as one group, five runs of each strategy, clang
18 with libc++ at `-O2`, tmux 3.7c, one idle Linux machine. They compare
wakeups rather than latency because all three see the same notifications; what
differs is how often the caller is woken to find nothing.

## What follows from it

**Output is a connection-construction choice, not a subscription.** The first
measurement settles it: a connection that starts silent cannot be made to
listen, so there is no per-pane opt-in to offer and pretending otherwise would
be an API that fails at runtime. Muting a pane on a listening connection is
the operation tmux actually supports.

**Owning your event loop is free.** `notification_fd` costs the same wakeups as
`wait_for_notifications`, so a caller with a `poll` of their own gives up
nothing by integrating there rather than parking a thread in a blocking call.
What is not free is asking repeatedly: a sleep-and-take loop pays about a
hundred times the wakeups for the same events.

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
