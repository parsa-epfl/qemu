---
name: multi-node
description: QFlex multi-node from the qemu (timing fork) perspective — in-sync copy of net/pdes-*.c, virtual time sourced from Flexus cycle count via the middleware tick (NOT icount), honours pause/resume from PDES, snapvm-external for per-node CPU+memory checkpoint. Use when the user asks about PDES code in this fork, multi-node during the timing phase, pause/resume coordination with Flexus, or how this fork differs from parallel-qemu for multi-node.
---

This skill's content lives in `MULTI_NODE.md` next to this directory's `CLAUDE.md`. Read it now: [../../../MULTI_NODE.md](../../../MULTI_NODE.md).

That doc covers what the timing QEMU fork contributes to PDES, how it leans on the middleware and Flexus for the virtual-time path and pause handshake, and links to the sibling `MULTI_NODE.md` files for the cross-cutting view.

## Incremental external snapshots (snapvm-external)

Per-sampling-unit checkpoints are **incremental**: a base memory image `<base>.mem/` (e.g. `init_warmed.mem`) + per-snapshot deltas `snapshot_<idx>.{loc,state.zstd}`, on top of a qcow2 *internal* snapshot entry that carries the disk. Loaded with `-drive …,snapshot=on,tmp-snapshot-name=snapshot_<idx>` + `-loadvm snapshot_<idx>,on-demand`. The custom `tmp-snapshot-name` option (`block.c` `bdrv_append_temp_snapshot`) loads the named snapshot's disk into the read-only base **and fakes a same-named empty snapshot in the transient overlay** so `bdrv_all_has_snapshot` passes; VM memory then streams on demand from the external files (`migration/savevm.c` `load_snapshot` / `incremental_snapshot_context`). Built `--enable-snapvm-external`. `tmp-snapshot-name` originated here and was later ported into parallel-qemu so a fully-phantom node can load these checkpoints too.
