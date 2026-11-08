# Shader precompilation

Goal: stop mid-game pipeline compilation stalls by porting switch2mac's shared pipeline key list to
Lichii. A per-title list of shader content hashes plus fixed pipeline state is recorded while the
title plays, then replayed into the pipeline cache on background workers before the game asks for
those pipelines. Lists carry identifiers only: no shader code, SPIR-V, environments, driver data or
`vulkan.bin`, so they stay shareable.

## Files

| File | What it is |
|---|---|
| [`learnings.md`](./learnings.md) | Measurements and findings, oldest first. Read this before changing the design. |
| [`todo.json`](./todo.json) | The plan (TideTable): phases, statuses and a changelog per task. |

## What works today

- **Pipeline key lists.** Every pipeline the title builds is recorded into a per-title list, and
  listed rows are rebuilt on background workers at boot. Verified on OoT. A session also writes
  `keylist.contribution.bin` with only the rows the loaded list was missing, so collecting for a
  shared list is a merge (`tools/nx2/keylist_tool.py merge`) rather than a whole-file replacement.
- **Budget eviction.** The pipeline cache keeps an LRU memory budget (1536 MB by default) instead of
  doing mass evictions on idle frames. Measured on OoT: 0 evictions in four minutes against 175
  before. `EDEN_NX2_PIPELINE_EVICT=legacy` restores the old policy (diagnostic switch, does not
  merge).
- **Static inventory.** `tools/nx2/shader_scan` extracts the shader programs inside a NUP container
  and writes a manifest. OoT's dump holds 39,392 unique NVN2 programs (36,176 BNSH plus BAHS, NDSR
  and NDVE); an offline sweep lowers 38,801 of them, 1,776 compile and pass `spirv-val`, 68 refuse
  to lower, and 521 need runtime context. `.bphsh`/`.bphsc`/`.bphnm` are Havok physics, not
  shaders.
- **List tooling.** `tools/nx2/keylist_tool.py` inspects, merges and diffs lists (12 tests), and
  `tools/nx2/lower_operand_audit.py` reports the opcodes the lowering handles but the
  register-pressure pass cannot yet shadow.

Static pre-lowering is not the delivery mechanism: lowering specialises on runtime uniform values
and storage pointer types, so a key list of built pipelines, not a precompiled shader archive, is
what gets shipped.

## Native frontend (phase 1.7)

Instead of rewriting each native NVN2 instruction into Maxwell encoding and letting the Maxwell
frontend translate it, a native frontend decodes the instruction and emits IR directly. That removes
the three failure classes the lowering keeps producing: the high-register shadow pass (two register
files competing for 255 slots), the per-form mode-word matching, and the encoding-expressibility
proofs (gather offsets, 64-bit carries). IR has no uniform register file, so UR0-UR63 become ordinary
SSA values rather than an R216-R245 remap.

It starts as a measurement, not a replacement: the lowering stays the default and the oracle,
and it keeps that role as the permanent fallback. Both frontends emit SPIR-V, so the choice
never depends on the host GPU: "native" means the guest instruction set, not the player's card.

```sh
build-sweep/bin/shader-sweep <program.bin> --stage compute --format instructions --native
# {"phase":"native","implemented_opcodes":66,"control_blocks":91,
#  "unimplemented_instructions":198,"unimplemented":[{"count":14,"opcode":"0x892"}, ...],
#  "status":"blocked","error":{"code":"translation_failed",...}}
```

The report gives both answers: the opcode histogram says what is missing overall, and building the
program for real says at which instruction the frontend actually stops. `control_blocks`,
`syntax_nodes` and `ir_instructions` show how far the structured IR came out; `structure_failed`
with `unsupported native opcode` means the control flow was fine and an instruction is missing.

A program with an empty `unimplemented` list is fully covered. Opcodes are added one family at a
time, each with a captured instruction and its nvdisasm decode.

To run the frontend for real, start the emulator with `EDEN_NX2_NATIVE=1`. The Vulkan pipeline cache
then tries the native frontend first and falls back to the lowering for any program it cannot
translate, logging `Native frontend: <hash> translated` or `... fell back (<reason>)` per shader. It
shares the stage setup and optimization passes with the Maxwell path (`Maxwell::FinalizeProgram`), so
a translated program is not a second pipeline. The OpenGL renderer is not wired.

## Diagnostic switches

All are env flags, off by default unless noted, and none of them merge to `nx2` as a behaviour
switch: a proven effect ships unconditional.

| Switch | Effect |
|---|---|
| `EDEN_NX2_KEYLIST=0` | Stop recording and prefetching. The list is on by default. |
| `EDEN_NX2_KEYLIST_PREFETCH=0` | Keep recording, but do not build listed state variants early. |
| `EDEN_NX2_PIPELINE_EVICT=legacy` | Old idle-frame mass eviction instead of the LRU budget. |
| `EDEN_NX2_PIPELINE_BUDGET_MB=<n>` | Pipeline cache memory budget, default 1536. |
| `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` | Build every cached pipeline at boot, as before the key list. |
| `EDEN_NX2_PIPELINE_DIFF=1` | Log what splits a new pipeline from an earlier one that shares its shaders. |

## Open work

- 1.3.2 load title shader programs at boot for key-list builds.
- 1.3.3 close the lowering gaps the OoT sweep found: 6 of the 68 programs that refused to lower now
  lower completely. The remaining groups are the multi-tap gather proofs, the uniform LEA carry,
  call linkage and a handful of opcodes that need a decode first. Findings and the exact instruction
  for each group are in [`learnings.md`](./learnings.md).
- 1.3.4 describe every opcode the lowering handles in the register-pressure pass (130 of 234 are
  still missing, which rejects a program whenever a high guest register forces a shadow copy).
  `tools/nx2/lower_operand_audit.py` prints the gap.
- 1.4.2 finish the merge workflow.
- 1.7 the native frontend: the scaffold, the coverage scoreboard, the arithmetic families
  (FMUL, FADD, FFMA, immediate SHFL) and uniform registers and predicates as SSA values are in
  place. Gathers, atomics and the parity gate follow (1.7.4 to 1.7.6).
- 1.5.1 measure end-to-end on OoT and one state-heavy title; 1.5.2 remove the experiment switches
  and document the shipped behaviour.

The debug server that drives these measurements (pause, step, input, memory) is documented in
[`../nx2/debug-mcp.md`](../nx2/debug-mcp.md).
