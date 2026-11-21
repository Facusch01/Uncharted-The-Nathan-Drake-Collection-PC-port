# Handover: U3 co-op freeze (stuck job counter safety net)

Written by Claude (Claude Code) for the maintainer, to go with the PR `fix/u3-stuck-job-counter`.
Image offsets below are guest offsets in the U3 1.02 executable (`boot-linked.bin` file offset = guest + 0x257958).

## 1. The symptom

- **Where:** Syria co-op adventure, now and then; also seen once in Yemen. Never seen in Borneo, London, Monastery, Airstrip or Fort.
- **When:** the player often reports a kill, a roll and a weapon switch at the same moment.
- **What:** the frame stops and the game never recovers. The hang watchdog and `u3mp-freeze.txt` both fire.

## 2. What the freeze looks like (from the runtime's own reports)

- **Parked jobs:** several jobs are parked in the job system's dependency table, each waiting for a counter to reach 0 ("exactly 0"). Those counters sit at 1, 2 or 4.
- **Fresh frame counters:** the stuck counters were made in the frame the freeze began. Most come from the particle renderer's per-frame counters, made at 0xf69d89 to 0xf69f60, plus a few others such as 0xea5d7c, 0xec9173 and 0x111b24f.
- **Nothing left to lower them:** in every queue, every priority has 0 jobs (`job_queue_counts`), and all six NdJobWorkerThreads are idle. A per-counter count of submitted-but-unfinished jobs (added for this work, see §4) is **0** for the stuck counter. No job that would lower it is queued, running or parked.
- **`settle_negative` doesn't apply:** these counters are above 0, not below.
- **Negative counters nearby:** a counter going negative often shows up shortly before (for example -5, -26). `settle_negative` sets it to 0. Seeing -N on one counter and +1 stuck on another at the same moment *suggests* some jobs lower the wrong counter, but this is **not proven**.

## 3. Job system facts used (all observed in the 1.02 image)

| What | Where |
|---|---|
| Job manager pointer | 0x1808f00 |
| Counter pool | manager+0xae0: lock +0x00, used-bitmap +0x20 (32 qwords), counters +0x120, capacity (u32) +0x128. 0x80 bytes each, value at +0x20 |
| Counter make | 0xd96620 (initial value via xchg at 0xd96716). 0xd96f60 also takes a counter from the pool itself (inline at 0xd970d5) |
| Counter give-back | 0xd967c0. Wait-then-give-back: 0xd975b0 |
| Job list | +0x04 write index, +0x08 job count, +0x10 jobs (0x40 bytes each), +0x18 capacity. Each job names the counter it lowers at +0x10 |
| Adding jobs to a list | 0xd96de0 (also inlined in callers); jobs past capacity are dropped without notice |
| Queue push | 0xda57d0 (rdi = list, sil = 1 append / 0 front). Called with 1 by the submits (0xd96d5b in 0xd96c60, 0xd973e5 in 0xd96f60); with 0 by the dependency release (0xda5f96, 0xda60a5) |
| Job finish lowering | lock xadd at 0xda80bb in the worker loop (counter = [rbp-0x78] = job+0x10), then 0xda5c20(counter, new value) releases waiting jobs |
| Counter update API | 0xd969c0(counter, value): takes the job lock (manager+0xd50) and calls 0xda5c20 |
| Thread wait loop | 0xd97480 (sleeps 1 ms per check). Job waits park through 0xda84f0; `counter_wait_log` already hooks both |

## 4. The fix in this PR (`gow3_pc/src/u3fix/stuck_counter.cpp`)

1. **Count jobs per counter.**
   - **+1 per job at the queue push** (append only). A list pushed to the front holds jobs released from the dependency table; those were already counted when their list was submitted.
   - **-1 at the lowering at 0xda80bb.** Indexed by the counter's slot in the pool.
2. **Detect a wait that can't end.** The `counter_wait_log` checker thread runs once a second over the recorded parked waits. It asks for a settle when all of these hold:
   - the wait is for 0 (or for at most a value of 0 or more);
   - the counter is above 0;
   - the wait has lasted at least 4 s;
   - the counted jobs on the counter are 0;
   - every queue is empty;
   - there has been no frame for 3 s (the particle renderer's game-jobs check at 0xeb9ca0 runs once a frame and is hooked only to time-stamp frames).
3. **Settle on a safe thread.** `counter_wait_sleep`, a game thread sleeping in the thread wait loop that holds no job lock, runs the queued settles. For each counter, it CASes the value to 0 and calls 0xd969c0(counter, 0), exactly what a job finishing on it would have triggered. The waiting jobs go back to the queues.
   - In practice a thread waits on counter 0x...5600 in this loop for the whole session, so the settles run within about 1 ms.
   - If no thread is in the loop, nothing is settled and the game behaves as before.
4. **Logging:** each settle is printed (first 50, then every 100th) with the waiting code's return addresses.

**Cost:** an atomic add per job, in the push hook and the lowering hook. Nothing else runs unless a wait already looks stuck.

## 5. Testing so far

- **0.10.118, diagnostic build with the same logic:** a full Syria co-op run as host (stage 27/28).
  - One freeze began at about 12 minutes. The counter (made at 0x111b24f, waited on at 0x111b3cb) was at 1 with 0 counted jobs, queues empty.
  - It was settled; the player saw about a 2 s pause and played on to the end.
- **This clean branch on 0.10.121:** builds. The four byte patterns (push, lowering, frame check, counter update) each match once. **Not yet played.**

## 6. Open questions / next steps

- **Root cause still unknown.** The fix makes the game recover; it doesn't stop the job from going missing. Two candidates fit the data:
  - **(a) The job is never submitted.** For example it is dropped by 0xd96de0 when a list is full, or its list is never pushed.
  - **(b) The job is submitted but lowers another counter.** That would match the -N counters seen just before.

  To tell them apart, log the list/job that should have lowered the stuck counter: who made the counter, and whether any job naming it was ever pushed.
- **The ndgi assertion.** `pComputeContext->Closed()` (ndgi-orbis-device.cpp:2102, EnqueueContext) appeared right before three earlier freezes and in no clean session. Possibly related.
- **A new crash, possibly related (not caused by this PR as far as I can tell):** one long session on the diagnostic build crashed in Fort at 0xebc1b6.
  - A particle job (callers 0xf721a9 / 0xf70e2a) was called with an empty range (start 0, count 0) and read element -1.
  - It may be the same race (particle data used before it is ready).
  - That session had extra experiments (counters kept out of the pool), so it is not a clean data point for this PR.
- **Measurement artifact:** `job_trace.cpp`'s "lowered by a job after it was freed" report over-counts. It doesn't see counters taken inline by 0xd96f60, so a reused counter looks freed. Don't use that number as evidence.
- **Conflict risk:** if `job_trace` is ever installed together with this fix, both hook the lowering at 0xda80bb. One of them would need to call the other.
