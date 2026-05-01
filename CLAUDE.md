# CLAUDE.md — qemu/

This is a **submodule** of the QFlex simulator. The parent repo is at `..`; its [CLAUDE.md](../CLAUDE.md) describes the four-phase pipeline, sampling vocabulary, and binary layout. Read it for context; this file documents only what's specific to *qemu/*.

## What this submodule is

A **PARSA-EPFL fork of upstream QEMU**, built **with** `--enable-libqflex --enable-snapvm-external`. This is the **timing QEMU** — distinct from the "fast" fork at [../parallel-qemu/](../parallel-qemu/).

> **Important: this file documents only the QFlex-specific delta from upstream QEMU.** Don't re-document upstream QEMU here.

The job of this binary is mostly to commit instructions in lockstep with the [Flexus](../flexus/) timing model so the model can be verified — it embeds [middleware/](middleware/) which `dlopen`s Flexus and forwards execution.

## Role in QFlex

**Phase 4 (timing simulation) only.** Once [WormCacheQFlex](../WormCacheQFlex/) has produced per-sampling-unit checkpoints during functional warming, this binary is invoked once per unit (the parent splits work into partitions for parallelism). For each unit it:

1. Loads the FW checkpoint (via `snapvm-external`).
2. Runs Flexus's per-unit detailed-warming prefix.
3. Runs the actual measurement segment, with Flexus driving timing.

After build, parent stages this binary at `<parent>/qemu-saved/build/qemu-system-aarch64` and `set_up_folders()` copies it into per-experiment `run/` as **`vanilla-qemu-system-aarch64`** ([../commands/config.py:298-303](../commands/config.py#L298)). "Vanilla" here means **non-parallel**, **not** unpatched — this binary is heavily patched.

## What's QFlex-specific

### `middleware/` sub-submodule

[middleware/](middleware/) contains `libqflex/` (the QEMU↔Flexus bridge) and `snapvm-external/` (file-based external snapshot save/load). See [middleware/CLAUDE.md](middleware/CLAUDE.md) for the API.

### Meson options and CONFIG_* macros

`--enable-libqflex` and `--enable-snapvm-external` are **Meson options**, not classic configure flags. Wiring is in [meson.build](meson.build):

- **Lines 1903–1923** declare the `middleware_option` map (`libqflex`, `snapvm-external`) and check for the corresponding subdirectories under `middleware/`. Either is required to be present if its option is enabled.
- **Line 2153** sets `CONFIG_LIBQFLEX` in `config-host.h` based on whether `libqflex` was found.
- **Line 2165** sets `CONFIG_SNAPVM_EXT` likewise.
- **Line 3252** lists `middleware` as a tracked subdir for code generation.
- **Lines 3388–3389** call `subdir('middleware')` if `middleware_include` is true — that's what actually compiles the shim.
- **Line 4378** prints a `PARSA` summary section after configure.

### QFlex-specific call sites in QEMU itself

- [softmmu/vl.c:3732](softmmu/vl.c#L3732) — calls `libqflex_init()`.
- [softmmu/vl.c:3737](softmmu/vl.c#L3737) — calls `snapvm_init()`.
  Both are guarded by the `CONFIG_*` macros and run late in QEMU startup.
- [softmmu/icount.c:43-44](softmmu/icount.c#L43) — `#ifdef CONFIG_LIBQFLEX` includes [middleware/libqflex/libqflex-legacy-api.h](middleware/libqflex/libqflex-legacy-api.h).
- [softmmu/icount.c:101](softmmu/icount.c#L101) — when `CONFIG_LIBQFLEX` is on, the icount path accumulates instruction counts rather than directly updating `qemu_icount`. This is what lets Flexus drive timing.

### `net/pdes-*.c` — PDES networking

The same multi-node PDES networking stack as in [../parallel-qemu/](../parallel-qemu/), including the Wisconsin Wind Tunnel (WWT) synchronisation protocol in `net/pdes-wwt.c`. The two forks are kept in sync on these files. See [../parallel-qemu/CLAUDE.md](../parallel-qemu/CLAUDE.md) for the file-by-file breakdown.

## Build

From the parent repo:

```sh
make qemu-config MODE=release          # or MODE=debug
make qemu-build MODE=release           # MODE must match what was passed to qemu-config
```

This passes `--enable-libqflex --enable-snapvm-external` to configure. Output: `build/qemu-system-aarch64`. Parent stages it under `qemu-saved/build/`.

## Public interface to the rest of QFlex

- **To Flexus:** via [middleware/libqflex/libqflex-legacy-api.h](middleware/libqflex/libqflex-legacy-api.h) — bidirectional in-process function-pointer API. See [middleware/CLAUDE.md](middleware/CLAUDE.md) for the full surface.
- **To the QFlex orchestrator:** standard QEMU CLI plus Flexus shared-object paths. The parent's `set_up_folders()` symlinks `/home/dev/qflex/kraken_out/lib{knotty,semi}kraken.so` (hard-coded — see [../commands/config.py:319-326](../commands/config.py#L319)).
- **To checkpoints:** `snapvm-external` exposes save/load APIs that bypass QEMU's internal snapshot path (file-based, not block-device-internal). Used to import FW checkpoints from [../WormCacheQFlex/](../WormCacheQFlex/) into the timing phase.

## Conventions and gotchas

- **All recent commits are PARSA-specific** — icount-vs-Flexus reconciliation, multi-node engine load races, deadline warping, etc. Don't `git pull --rebase` from upstream QEMU without expecting heavy conflicts.
- **Two forks, kept in sync on PDES.** Changes to `net/pdes-*.c` here likely need to be mirrored in [../parallel-qemu/](../parallel-qemu/) and vice versa.
- **The middleware sub-submodule has a `.gitmodules` quirk** — see [middleware/CLAUDE.md](middleware/CLAUDE.md). Don't blindly `git submodule update --remote`.
- **Hard-coded `/home/dev/qflex/kraken_out/` path** in the parent assumes the in-container layout — works in the dev image, breaks on the host.
- The submodule lives on branch `feature/multi-node`.

## See also

- [../CLAUDE.md](../CLAUDE.md) — qflex root: four-phase pipeline, `ExperimentContext`, build commands, multi-node config.
- [middleware/CLAUDE.md](middleware/CLAUDE.md) — the embedded QEMU↔Flexus shim. Read this for the actual API surface (`FLEXUS_API_t` / `QEMU_API_t`).
- [../flexus/CLAUDE.md](../flexus/CLAUDE.md) — the timing model that this QEMU `dlopen`s via the middleware.
- [../parallel-qemu/CLAUDE.md](../parallel-qemu/CLAUDE.md) — the *other* QEMU fork (fast). PDES networking is kept in sync between the two.
- [../WormCacheQFlex/CLAUDE.md](../WormCacheQFlex/CLAUDE.md) — produces the FW checkpoints this QEMU loads via `snapvm-external` at the start of the timing phase.
