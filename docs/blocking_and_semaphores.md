# Blocking, waking, and the first semaphore — Stage 5a

Every task through Stage 4 was always runnable. The scheduler's only job
was deciding *whose turn* it was among tasks that all had something to
do. This stage introduces the other half of what a real scheduler has to
handle: a task that has genuinely nothing to do right now, and shouldn't
be given a turn until something changes that.

## Why the scheduler needs to know "ready" vs "blocked"

Through Stage 4, `kernel/scheduler.c`'s round-robin logic just cycled
through every registered task in order — there was no concept of a task
that shouldn't be picked. The moment a task can call something like
`sem_wait()` and have nothing available, that stops being true: giving
it a turn anyway would mean running code that immediately says "still
nothing to do" and gives the turn right back, over and over, as fast as
the scheduler can switch — busy-waiting, exactly the thing blocking
exists to avoid.

So `TCB_t` (`kernel/kernel.h`) gains a `state` field:

```c
typedef enum {
    TASK_READY   = 0,
    TASK_BLOCKED = 1,
} task_state_t;
```

and `kernel/scheduler.c`'s task-picking logic (`pick_next_ready()`) skips
any `TASK_BLOCKED` task entirely when deciding who runs next. That one
field, checked in one place, is the entire mechanism by which "blocking"
removes a task from the rotation — no second data structure needed to
track who's eligible.

## The waiter list — why an intrusive linked list

When a task blocks, *something* needs to remember it, so whoever
eventually satisfies the wait knows who to wake. The obvious option is a
fixed-size array per semaphore (`TCB_t *waiters[MAX_TASKS]`), but that
duplicates an arbitrary capacity constant everywhere something can be
waited on. Instead, `TCB_t` gets one more field:

```c
struct TCB *next_waiter;
```

and a semaphore's entire "who's waiting" state is just one pointer — the
head of a chain threaded directly through the TCBs themselves:

```
sem->waiters -> task_X -> task_Y -> NULL
                (via task_X->next_waiter) (via task_Y->next_waiter)
```

This is called an *intrusive* linked list because the "next" pointer
lives inside the object being linked, rather than in a separate node
allocated just for list bookkeeping. It's how real kernels (FreeRTOS
included) do this, for the same reason: no allocator, no fixed capacity,
no extra memory beyond the TCBs that already exist. A task can only ever
be on one wait list at a time, which is exactly what one `next_waiter`
field per TCB allows and no more.

## `scheduler_yield()` — one door, two reasons to walk through it

Stage 4's `SysTick_Handler` used to pick the next task directly. Stage 5a
pulls that decision out into its own function:

```c
void scheduler_yield(void);
```

`pick_next_ready()` (`kernel/scheduler.c`) does the actual search: walk
`task_list` starting right after the current task, in round-robin order,
skipping anyone `TASK_BLOCKED`, until a `TASK_READY` task is found — or
until the search comes all the way back around to the current task's own
slot, which happens exactly when nobody else is eligible. `scheduler_yield()`
wraps that: if the "next" task turns out to be the current one, there's
nothing to switch to, so it's a safe no-op (no pointless save/restore
through PendSV). Otherwise it does exactly what Stage 3/4's
`kernel_switch_to()` always did: set `next_task` and pend `PendSV`.

Two different callers now go through this one function:

- `SysTick_Handler`, when a time slice expires — *forced*, periodic
  rescheduling, same as Stage 4.
- `sem_wait()` (below), when a task has nothing to do — *voluntary*,
  immediate rescheduling, new in Stage 5a.

Funneling both through one function is the same mechanism/policy
discipline Stage 4 established: "who runs next" is decided in exactly
one place, no matter what event triggered the need to decide.

### The one real limitation this creates: no idle task yet

`pick_next_ready()` can legitimately find *nobody* ready — if every
single task, including the one calling `scheduler_yield()`, is
`TASK_BLOCKED`. A real RTOS handles this with an "idle task" — a task
that's never blocked, always available as a last resort, usually just
sitting in a low-power sleep instruction. This project doesn't have one
yet; that's explicitly Stage 10 on the roadmap. Until then,
`scheduler_yield()` just traps (spins forever) if it ever finds nothing
runnable, and anything built on top of the scheduler has to guarantee
that can't happen. Stage 5a's demo (`app/main.c`) does this by keeping
`task_producer` permanently non-blocking — worth knowing as a real,
temporary constraint, not something papered over.

## The semaphore itself

`kernel/sem.h` / `kernel/sem.c`:

```c
typedef struct {
    volatile int32_t count;
    TCB_t *waiters;
} sem_t;

void sem_init(sem_t *sem, int32_t initial_count);
void sem_wait(sem_t *sem);
void sem_post(sem_t *sem);
```

`count` is how many "units" are available to take without blocking.
`waiters` is the intrusive wait list described above. The invariant
worth stating precisely: **`count` and `waiters` are never both active
at once.** Either there are unclaimed units sitting in `count` (nobody's
asked for them yet), or there are tasks queued in `waiters` (there was
nothing left in `count` when they asked). `sem_post()`'s logic is built
specifically to preserve that.

### `sem_wait()` — fast path and slow path

```c
void sem_wait(sem_t *sem)
{
    cpsid i;                              /* critical section - see below */
    if (sem->count > 0) {
        sem->count--;
        cpsie i;
        return;                           /* fast path: no blocking at all */
    }
    current_task->state = TASK_BLOCKED;
    current_task->next_waiter = sem->waiters;
    sem->waiters = current_task;
    cpsie i;
    scheduler_yield();                    /* slow path: give up the CPU */
}
```

If a unit's already available, this is just a decrement — no scheduler
involvement whatsoever, exactly as cheap as it should be for the common
case. Only when nothing's available does the task actually block: mark
itself `TASK_BLOCKED`, push itself onto the semaphore's waiter list, then
ask the scheduler for someone else. From the calling task's own point of
view, `sem_wait()` is a function call that sometimes returns instantly
and sometimes takes an arbitrarily long time — the same "returns later"
property Stage 3's `kernel_switch_to()` first introduced.

### `sem_post()` — direct hand-off, not a blind increment

```c
void sem_post(sem_t *sem)
{
    cpsid i;
    if (sem->waiters != 0) {
        TCB_t *woken = sem->waiters;
        sem->waiters = woken->next_waiter;
        woken->next_waiter = 0;
        woken->state = TASK_READY;        /* hand off directly - count untouched */
    } else {
        sem->count++;                     /* nobody waiting - bank it */
    }
    cpsie i;
}
```

If a task is already queued, wake it *directly* rather than incrementing
`count` and letting it go re-claim a unit on its own. This is what keeps
the "never both active" invariant true: if `count` were incremented too,
some unrelated third task calling `sem_wait()` before the woken task gets
scheduled could steal the unit that was meant for it. Direct hand-off
means the unit is already spoken for the instant `sem_post()` runs.

Notice what `sem_post()` does **not** do: it never calls
`scheduler_yield()` or `kernel_switch_to()`. Marking a task `TASK_READY`
only changes its *eligibility*. Whether it actually runs next, or waits
its turn behind whatever's currently executing, is still entirely the
round-robin scheduler's call — a sync primitive's job is deciding who's
*allowed* to run; the scheduler's job is deciding who *actually* does,
right now. Same separation of concerns as always, one layer further up.

## Why `cpsid i` / `cpsie i` — and why that's a stopgap

Both functions wrap their real work in `cpsid i` (disable all maskable
interrupts) ... `cpsie i` (re-enable). This is the bluntest tool
available: it doesn't just block a context switch, it blocks *every*
interrupt in the system for those few instructions. That's necessary
right now because `count`, `waiters`, and a task's `state`/`next_waiter`
all have to change together as one atomic unit — a `SysTick` tick landing
mid-update could otherwise observe (or create) a half-updated state, like
a task marked `TASK_BLOCKED` that never actually got linked into
`waiters` — lost forever, nothing able to wake it.

PRIMASK-wide masking is correct but heavy-handed: it also blocks
interrupts that have nothing to do with the scheduler. Stage 7 on the
roadmap replaces this with `BASEPRI`-based critical sections that only
block interrupts at or below the kernel's own priority. Not a
correctness fix yet — nothing above the kernel's priority exists in this
project so far — just a refinement for when it does.

## A subtle, harmless race worth knowing about

Between `sem_wait()`'s `cpsie i` and its call to `scheduler_yield()`,
interrupts are back on — so, in principle, `SysTick` could fire in that
one-instruction gap and force a switch away from this task on its own,
before the task's own `scheduler_yield()` call ever executes. That's
completely fine: `SysTick_Handler`'s own `scheduler_yield()` call would
correctly skip this task (its `state` is already `TASK_BLOCKED`) and pick
someone else. Later, when this task is eventually woken and rescheduled,
execution resumes right where it left off — about to call
`scheduler_yield()` itself — and does, redundantly, immediately giving
the CPU back up again for one extra scheduling round-trip before actually
proceeding. Wasteful by a few dozen cycles in a vanishingly rare timing
window; never incorrect. Worth knowing about, not worth fixing yet.

## The demo, and what a broken version would look like

`task_producer` never blocks: it just delays, then calls `sem_post()`
and increments `producer_posts`. `task_consumer` spends nearly all its
life blocked in `sem_wait()`; each time it's woken, it toggles the LED
and increments `consumer_wakes`.

- **LED never blinks at all** — `task_consumer` is never being woken.
  Check `sem_post()`'s waiter hand-off, or that `SysTick`/`PendSV`
  priorities are still correctly configured (`kernel_init()`).
- **`consumer_wakes` stays at 0 while `producer_posts` climbs** — same
  symptom, confirmed at the data level: waking isn't reaching the
  consumer task at all.
- **Both counters climb, but wildly apart with the gap only growing** —
  would suggest the woken task isn't actually getting scheduled once
  `TASK_READY` again; check `pick_next_ready()`'s skip logic.
- **Hard fault / freeze** — check the intrusive list wiring first
  (`next_waiter` reused incorrectly, or a stale pointer left over from
  the FIFO-vs-LIFO ordering) - see `sem_wait()`'s comment on wakeup order.

## Verifying it on real hardware

No single deterministic breakpoint works well here (same story as Stage
4) — the natural move is `continue`, then Ctrl-C to interrupt the
free-running target:

```
target extended-remote :3333
monitor reset halt
continue
```

Let it run a couple of seconds, Ctrl-C, then:

```
print producer_posts
print consumer_wakes
```

Both should be nonzero and close to each other (consumer briefly lags
producer by at most whatever's currently banked in `blink_sem.count`).
`continue` again, Ctrl-C again, and confirm both keep climbing together.
For the deepest look:

```
print blink_sem
print tasks[1].state
```

Between two producer posts, `tasks[1].state` (the consumer) should read
`TASK_BLOCKED` (1) far more often than `TASK_READY` (0) if you sample at
random moments — direct confirmation it's genuinely blocked most of the
time, not busy-waiting.
