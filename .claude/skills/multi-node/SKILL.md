---
name: multi-node
description: QFlex multi-node from the qemu (timing fork) perspective — in-sync copy of net/pdes-*.c, virtual time sourced from Flexus cycle count via the middleware tick (NOT icount), honours pause/resume from PDES, snapvm-external for per-node CPU+memory checkpoint. Use when the user asks about PDES code in this fork, multi-node during the timing phase, pause/resume coordination with Flexus, or how this fork differs from parallel-qemu for multi-node.
---

This skill's content lives in `MULTI_NODE.md` next to this directory's `CLAUDE.md`. Read it now: [../../../MULTI_NODE.md](../../../MULTI_NODE.md).

That doc covers what the timing QEMU fork contributes to PDES, how it leans on the middleware and Flexus for the virtual-time path and pause handshake, and links to the sibling `MULTI_NODE.md` files for the cross-cutting view.
