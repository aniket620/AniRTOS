# Task priorities and immediate preemption — Stage 5b

Stage 5a gave tasks a way to genuinely have nothing to do. It did NOT
give them any notion of importance — every task was equally worth the
scheduler's attention, and among tasks that were both ready, plain
round-robin decided whose turn it was. That's fine until two tasks are
ready at once and one of them matters a lot more than the other right
now. Stage 5b adds that missing dimension: priority.

## The convention, and the trap it's designed to avoid

```c
#define TASK_PRIORITY_LOW    0
#define TASK_PRIORITY_NORMAL 1
#define TASK_PRIORITY_HIGH   2
```

**Higher number wins.** A `TASK_PRIORITY_HIGH` task always beats a
`TASK_PRIORITY_LOW` task for the CPU, whenever both are `TASK_READY`.

This is worth dwelling on because it's the *opposite* convention from
one already used all over this codebase: ARM's own hardware exception
priorities (`SCB_SHPR3`, configured in `kernel_init()`) use **lower
number wins** — priority `0` is the most urgent hardware exception,
`0xFF` is the least. That's not a choice this project made; it's how the
Cortex-M architecture itself defines exception priority, and both
`PendSV` and `SysTick` are deliberately set to `0xFF` (the *lowest*
hardware priority) precisely so they never preempt anything more urgent.

Task scheduling priority, introduced fresh in this stage, uses the
*other* direction — higher number, more important — matching the more
common RTOS-textbook and FreeRTOS convention, because "bigger number,
bigger deal" reads more naturally in application code (`task_init(...,
TASK_PRIORITY_HIGH)` is clearer than handing it a `0` for "urgent"). The
two never get compared against each other directly, but it's a real trap
if you're skimming `kernel_init()`'s SHPR3 bit-twiddling and `task_init()`'s
priority argument in the same sitting and assume they mean the same
thing when they don't.

## Two-pass task selection

Stage 5a's `pick_next_ready()` (`kernel/scheduler.c`) did one job: find
the next `TASK_READY` task, round-robin, skipping `TASK_BLOCKED` ones.
Stage 5b adds a pass in front of it:

```c
static int32_t pick_next_ready(void)
{
    /* Pass 1: what's the best priority currently on offer? */
    int32_t highest = -1;
    for (uint32_t i = 0; i < num_tasks; i++) {
        if (task_list[i]->state == TASK_READY && (int32_t)task_list[i]->priority > highest) {
            highest = (int32_t)task_list[i]->priority;
        }
    }
    if (highest < 0) return -1;

    /* Pass 2: round-robin among READY tasks that share that top priority. */
    for (uint32_t i = 1; i <= num_tasks; i++) {
        uint32_t idx = (current_index + i) % num_tasks;
        if (task_list[idx]->state == TASK_READY && (int32_t)task_list[idx]->priority == highest) {
            return (int32_t)idx;
        }
    }
    return -1;   /* unreachable: pass 1 guarantees a match */
}
```

Priority breaks ties *across* tiers (a priority-2 task always beats a
priority-0 task, full stop, no negotiation). Round-robin still breaks
ties *within* a tier (two priority-1 tasks that are both ready still
take fair turns with each other, exactly like Stage 4/5a). Nothing about
`scheduler_yield()`'s own logic needed to change — it already just asks
"is the answer my own index?" and no-ops if so, which still correctly
means "nothing outranks me right now," priority-aware or not.

## Why `sem_post()` had to change

Stage 5a made a point of saying `sem_post()` never forces a switch — it
only changes a task's eligibility, and the *scheduler* decides when
(and whether) to act on that, which in practice meant "maybe up to 20ms
later, whenever the next `SysTick` tick lands."

That's no longer good enough once priority exists. If a high-priority
task is asleep waiting for something urgent, and `sem_post()` wakes it
but then just... waits for the next tick to actually switch to it, the
entire point of calling it "high priority" evaporates — it's not
responding urgently, it's responding "eventually, on the next
scheduling tick," identical to how a low-priority task would be treated.
Priority has to mean something the instant it applies, not on a delay.

So `kernel/sem.c`'s `sem_post()` now does this after waking a task:

```c
if (woken != 0) {
    scheduler_yield();
}
```

Only on the path where a task was actually woken — never on the "just
bank a unit in `count`" path, since nothing's eligibility changed there.
And note what this does *not* require: `sem_post()` doesn't compare
`woken`'s priority against the currently running task's priority itself.
It doesn't need to — `scheduler_yield()` already re-derives "who's
actually the best choice right now" from scratch via `pick_next_ready()`
every time it's called, and already has a built-in no-op for "the answer
is still me." Asking the one function that already knows how to answer
"who should run right now" is simpler and more correct than trying to
duplicate that logic here.

One honest, minor wrinkle: if `woken` turns out to be the *same*
priority as whoever's currently running (not strictly higher), this can
still trigger an immediate switch to it — slightly different from strict
textbook/FreeRTOS semantics, where only a STRICTLY higher priority wake
preempts immediately, and an equal-priority wake just joins the back of
the line for its normal turn. Here it's harmless (it's still a
legitimate round-robin turn among equals, just taken a little early),
and not worth extra bookkeeping to special-case for a two-task demo —
worth knowing about if this scheduler ever needs to match another RTOS's
scheduling behavior exactly.

## The demo

`task_low` (`TASK_PRIORITY_LOW`) never blocks — it delays, posts to
`event_sem`, increments `low_runs`, and loops. It doubles as this
project's "someone must always be ready" task (see
`kernel/scheduler.c`'s no-idle-task caveat from Stage 5a — still true
here). `task_high` (`TASK_PRIORITY_HIGH`) spends nearly all its life
blocked in `sem_wait()`; the instant `task_low` posts, it should take
over immediately — not "soon," not "next tick" — toggle the LED, and go
straight back to waiting.

## Verifying immediate preemption, deterministically

Stage 5a taught a real lesson worth reusing here: chasing a specific
internal transition with random `continue` / Ctrl-C sampling is
unreliable, because real time passing between our messages lets state
drift before you get to look. A targeted breakpoint at the exact line
that matters is far more convincing — and this stage has a perfect
candidate: the `scheduler_yield()` call inside `sem_post()`
(`kernel/sem.c`, inside `if (woken != 0) { ... }`).

```
break sem.c:139
continue
```

(gdb resolves the line from the source itself, so this works regardless
of the exact compiled address.) When it hits, `task_low` is still the
one executing — check it:

```
print current_task
```

This should read `&tasks[0]` (`task_low`) — proving we're stopped
*inside* `sem_post()`, called *from* `task_low`, right before the switch
happens. Now step into the call:

```
step
```

A few more `step`s or a `next` will walk into `scheduler_yield()` and,
inside it, into `kernel_switch_to()` — and by the time control returns
from that call, `current_task` will already be `&tasks[1]` (`task_high`).
The key thing this proves that Stage 5a's counters alone couldn't: the
switch to the higher-priority task happens synchronously, as part of
`sem_post()` itself finishing its job — not asynchronously, "sometime
before the next tick." That synchronous, immediate hand-off *is*
priority-based preemption.
