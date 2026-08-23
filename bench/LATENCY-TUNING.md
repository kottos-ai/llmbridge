# Latency tuning the benchmark host

Reference for reproducing and interpreting `llmbridge`'s streaming latency numbers.
Read this before publishing any per-token latency figure, because **the single largest
term in the measured hop cost is a host power-management setting, not code.**

Everything here needs `root`. Nothing here is required to *run* the benchmarks; it is
required to know *what you measured*.


## Why this matters (measured, not theoretical)

At 64 concurrent streams the added latency of inserting a proxy into the path decomposes
roughly like this:

| path | p50 added | hop cost |
|---|---|---|
| direct to provider, no proxy | 52 µs |. |
| `nullrelay`. 2 hops, zero work | 106 µs | **54 µs** |
| `llmbridge`. 2 hops, full SSE translation | 108 µs | 56 µs |

llmbridge's own work is therefore small and the rest is the cost of the extra hop, which
is *mostly CPU idle-state exit latency*: the time for a core to climb out of a sleep
state to service the arriving token. Measured with idle states capped at C1, where the
noise floor is 4x lower, the same decomposition reads: floor 13 µs, hop 14 µs, and
**llmbridge's own work ~5 µs** (the ~2 µs implied by the untuned numbers above was inside
that configuration's noise; the tuned figure supersedes it).

Two measurements on the reference box (i7-9750H, `intel_idle` driver, `menu` governor):

| condition | hop cost |
|---|---|
| CPUs free to enter deep C-states | **54 µs** |
| package kept awake by a spinner on 2 cores | **39 µs** (−29%) |
| relay busy-polling (never sleeps) | **6 µs** (−89%) |

C-state exit latencies advertised by this CPU. Compare against the hop cost above:

| state | poll | C1 | C1E | C3 | C6 | C7s | C8 | C9 | C10 |
|---|---|---|---|---|---|---|---|---|---|
| exit µs | 0 | 2 | 10 | 70 | 85 | 124 | 200 | 480 | 890 |

Read your own box's values with:

```sh
paste <(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/name) \
      <(cat /sys/devices/system/cpu/cpu0/cpuidle/state*/latency)
cat /sys/devices/system/cpu/cpuidle/current_driver /sys/devices/system/cpu/cpuidle/current_governor
```

**Implication for published numbers.** Untuned, a large fraction of our reported
per-token latency is the host's idle governor, identical for every gateway in the
comparison. Tuning shrinks *all* paths, so the *relative* result is unaffected, but the
absolute figures are only meaningful alongside the tuning state, which is why
`BENCHMARKS.md` records it.


## Warning: Do not leave this tuning applied; it heats the machine

Capping idle states stops cores sleeping, and the `performance` governor pins clocks at
maximum. On a thermally-limited laptop that raises the *idle* package temperature from
~60 C to ~80 C, which eats the turbo headroom a throughput benchmark depends on:

| | idle temp | max clock | saturation throughput |
|---|---|---|---|
| tuned (`-D 5` + performance) | ~80 C | 4002 MHz | ~75k RPS |
| defaults (`-E` + powersave) | ~60-64 C | 4289-4396 MHz | **~81.6k RPS** |

So the tuning helps *latency* measurements (it removes the C-state exit from every
wakeup) and hurts *throughput* measurements. Apply it for the streaming latency runs,
restore defaults for saturation runs, and restore defaults when you are done:

```sh
sudo cpupower idle-set -E && sudo cpupower frequency-set -g powersave
```

## Method 1. `cpupower` (no reboot, instantly reversible) ← recommended for A/B runs

Sets a PM-QoS latency target: every idle state whose exit latency exceeds the target is
disabled. `-D 5` keeps `POLL` and `C1` (2 µs) and kills `C1E` (10 µs) and everything
deeper. This is the realistic production tuning.

```sh
sudo cpupower idle-set -D 5     # disable stat1es with exit latency > 5 us
sudo cpupower idle-set -E       # RESTORE: re-enable all idle states
sudo cpupower idle-info          # show what is currently enabled
```

`-D 0` is the extreme (leaves only `POLL`; cores never sleep). Use it to bound the
effect, not as a standing configuration.

If `cpupower` is missing: `sudo apt install linux-tools-common linux-tools-$(uname -r)`.

**Variant 1a: surgical, via sysfs.** Disable individual states by index, no `cpupower`
needed. Indices follow the `state*/name` order printed above; on this box `state3` is C3.

```sh
# disable C3 and deeper on every CPU
for c in /sys/devices/system/cpu/cpu*/cpuidle; do
  for s in "$c"/state[3-8]; do echo 1 | sudo tee "$s/disable" >/dev/null; done
done
# RESTORE
for s in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable; do echo 0 | sudo tee "$s" >/dev/null; done
```

**Variant 1b: hold `/dev/cpu_dma_latency` open.** This is the mechanism `cpupower -D`
uses. The target applies only while the file descriptor stays open, so it self-reverts
when the process exits. That is the safest option of all, and it needs no tooling.

```sh
# hold a 0 us latency target for 600 seconds, then release automatically
sudo bash -c 'exec 3<>/dev/cpu_dma_latency; printf "\x00\x00\x00\x00" >&3; sleep 600'
```


## Method 2. GRUB kernel parameters (persistent, needs two reboots)

More thorough than method 1: the deeper states disappear from sysfs **entirely** rather
than being flagged disabled, so no governor can select them. The right choice for a host
dedicated to low-latency measurement, and a liability on a laptop you also use for
other things, because it persists until you undo it.

```sh
sudo nano /etc/default/grub
```

Append to the **existing** `GRUB_CMDLINE_LINUX_DEFAULT` (keep whatever is already
there, e.g. `quiet splash`):

```
intel_idle.max_cstate=1 processor.max_cstate=1
```

`intel_idle.max_cstate=1` is the operative one when the driver is `intel_idle` (check
`current_driver` above). `processor.max_cstate=1` is belt-and-braces: it covers the
`acpi_idle` fallback in case `intel_idle` is bypassed or disabled.

```sh
sudo update-grub
sudo reboot
```

Verify after reboot; only `POLL C1` should remain:

```sh
cat /sys/devices/system/cpu/cpu0/cpuidle/state*/name
cat /proc/cmdline
```

**Revert:** delete those two tokens from `/etc/default/grub`, `sudo update-grub`,
reboot again.


## Method 3 (`idle=poll` (GRUB)) not on a laptop

```
idle=poll
```

Cores never enter *any* idle state; the idle loop spins. Lowest possible wakeup latency,
but every core draws full power continuously. On a mobile part such as the i7-9750H this
means constant fan and thermal throttling, which **lowers clocks and therefore degrades
the very latency being measured.** Only defensible on a desktop or server with headroom.


## Harness prerequisite: the listen / SYN queue (run this before any head-to-head)

```sh
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=8192 net.core.somaxconn=8192
```

**When:** before any benchmark where clients open connections at a high rate, which is
every streaming head-to-head, because the load generator opens a new connection per
stream. Both settings revert on reboot, so there is nothing to undo.

**Why this is not optional.** The defaults on this box are `tcp_max_syn_backlog = 1024`
and `somaxconn = 4096`. Above roughly 256 concurrent streams the mock's accept queue
overflows, the kernel silently drops SYNs (`tcp_abort_on_overflow = 0`), and connects
stall or fail. That makes the *harness* the limiter instead of the software under test.

It is worse than merely noisy, because **it does not penalise both sides equally**:
`llmbridge` pools its upstream connections and barely notices, while a gateway that
opens a fresh upstream per request absorbs the full cost. A run in that state reports a
competitor's connect failures as if they were its latency. One measured run took **18
LiteLLM connect failures during measurement** and had to be discarded entirely.

**Always verify afterwards**: the count must not move during the run:

```sh
nstat -az | grep ListenOverflows      # snapshot before
# ... run the benchmark ...
nstat -az | grep ListenOverflows      # must be UNCHANGED; if it grew, the result is void
```

A non-zero delta invalidates the run no matter how plausible the numbers look. Raising
the queue buys headroom but does not make the mock's single accept loop faster, so at
high enough concurrency this will bite again; the fix then is a narrower comparison or a
multi-threaded accept path, not a bigger number here.

## Related knobs worth recording (not yet applied here)

Not needed for the C-state result, but they belong in a published environment spec:

```sh
# pin the frequency governor so turbo ramp-up does not appear as latency
sudo cpupower frequency-set -g performance

# disable turbo entirely for run-to-run determinism (Intel P-state)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

# check SMT topology: spinning threads on sibling hyperthreads contend for
# execution ports, so the usable busy-poll budget is PHYSICAL cores, not "CPUs"
lscpu | grep -iE 'core\(s\) per socket|thread\(s\) per core|^cpu\(s\):'
```


## The alternative to tuning the host: busy-poll in the gateway

Host tuning shrinks the wakeup; **busy-polling removes it.** A worker that never blocks
is already on-CPU when a token arrives: no idle-state exit, no scheduler dispatch. That
took the hop from 54 µs to **6 µs** in the `nullrelay` prototype, and needs no root and
no host reconfiguration.

The trade-off is a whole core per spinning worker, and spinning is **not divisible**: N
busy-polling workers need N cores, because each must spin independently. Oversubscribe
and it inverts: a token can land while your spinner sits off-CPU awaiting its scheduler
turn, which is worse than blocking. Budget by **physical cores minus one** (leave one for
the OS and network softirq), not by the CPU count `nproc` reports.

**Status: measured in a prototype, not a shipped option.** The 6 µs figure comes from a
patched `nullrelay` used as a diagnostic; `llmbridge` has no busy-poll flag and its loops
always block. Host tuning (methods 1–3 above) is the supported way to shrink the wakeup
today. See `BENCHMARKS.md` for the measured comparison.
