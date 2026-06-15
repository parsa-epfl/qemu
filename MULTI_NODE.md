# MULTI_NODE.md — qemu/ (timing fork)

This file documents what this QEMU fork (the **timing** binary, `vanilla-qemu-system-aarch64` once staged) contributes to QFlex multi-node and how it leans on the other submodules. The cross-cutting overview is in [../MULTI_NODE.md](../MULTI_NODE.md). Read that first if you haven't.

For this submodule's general context (role, build, layout) see [CLAUDE.md](CLAUDE.md).

## What this submodule contributes to PDES

- **Carries an in-sync copy of `net/pdes-*.c`** from [../parallel-qemu/](../parallel-qemu/). Same singleton `PDESEngine`, same `PDESWWT`, same wire format, same shm naming. The two forks must stay aligned on these files.

- **Provides virtual time during phase 4 (timing) — but the clock source is not icount.** Quantum args here are `-quantum size=<quantum_size>,check_period=…` (see [../commands/qemu.py:101](../commands/qemu.py#L101)), and the actual ns-per-tick comes from **Flexus's cycle count** via the middleware `tick` callback. PDES asks "what time is it?"; the answer ultimately came from [../flexus/](../flexus/) through [middleware/](middleware/). This is *the* multi-node distinction between the two forks — same PDES, different clock source.

- **Honours pause/resume from PDES.** When WWT decides this node has run past a sync boundary, the engine asks the middleware to pause; the middleware pauses both QEMU's main loop and Flexus. The hooks for this are in `softmmu/icount.c` (around line 101 — the `CONFIG_LIBQFLEX`-guarded path that lets icount accumulate from Flexus rather than self-update) and in `softmmu/vl.c` (the `libqflex_init`/`snapvm_init` startup at lines 3732/3737).

- **`snapvm-external` is the per-node CPU+memory checkpoint mechanism** that participates in distributed snapshots — file-based, bypassing QEMU's internal block-device-internal snapshot path. Wired in via [middleware/snapvm-external/](middleware/snapvm-external/) when `--enable-snapvm-external` is set.

## How this submodule uses the other submodules

- **[middleware/](middleware/)** (sub-submodule) — embedded into the build via `subdir('middleware')` ([meson.build:3389](meson.build#L3389)) when `--enable-libqflex` and `--enable-snapvm-external` are set. The middleware is what:
  - turns Flexus's `tick` callback into the value PDES reads as virtual time, and
  - exposes `pause`/`resume`/`is_paused` (in `FLEXUS_API_t`) so PDES can halt the timing model.

- **[../flexus/](../flexus/)** — `dlopen`'d via the middleware ([middleware/libqflex/libqflex-module.c:111](middleware/libqflex/libqflex-module.c#L111)). Flexus *is* the clock source. Flexus must implement pause/resume/is_paused for PDES coordination to work; that's what its `features/multi-node` branch has been delivering.

- **[../parallel-qemu/](../parallel-qemu/)** — the other fork. PDES `net/pdes-*.c` files are mirrored between the two; **changes here usually need to be replicated there** (or vice versa), otherwise FW and timing drift apart on the wire format or sync protocol.

- **[../WormCacheQFlex/](../WormCacheQFlex/)** — does **not** load it. WormCache is a parallel-qemu plugin only. This fork only consumes the FW checkpoints WormCache produced — and those are loaded via `snapvm-external` at the start of each sampling unit.

## Phase-specific PDES invocation

The timing fork is launched per sampling unit (or per partition of sampling units) by the run scripts produced during the partition phase. Quantum args:

```
-quantum size=<quantum_size>,check_period=...
```

The PDES backend wiring (`-netdev pdes,…` + the matching guest NIC) comes from the same `setup_nic_args()` ([../commands/config.py:351-396](../commands/config.py#L351)) that parallel-qemu uses, so per-link config is identical.

## See also

- [../MULTI_NODE.md](../MULTI_NODE.md) — the comprehensive cross-cutting overview.
- [../parallel-qemu/MULTI_NODE.md](../parallel-qemu/MULTI_NODE.md) — owner of the canonical PDES code; FW-phase clock source.
- [middleware/MULTI_NODE.md](middleware/MULTI_NODE.md) — pause handshake and Flexus `tick` → virtual time (lives inside this submodule).
- [../flexus/MULTI_NODE.md](../flexus/MULTI_NODE.md) — the cycle-count clock that drives PDES in the timing phase.
- [../WormCacheQFlex/MULTI_NODE.md](../WormCacheQFlex/MULTI_NODE.md) — the FW plugin that produces the checkpoints `snapvm-external` ingests.
