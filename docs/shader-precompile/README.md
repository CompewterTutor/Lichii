# Shader precompilation

Goal: stop mid-game pipeline compilation stalls. The delivery is the shared shader cache v2: a
title's pipeline is recorded as the guest environments its shaders were translated from, held in an
append-only log next to the old versioned cache, with a driver layer on `VK_KHR_pipeline_binary`, and
`shader_warm`/`PrecacheShaders` build a title's pipelines before the game asks for them.

An earlier design recorded a per-title key list of shader hashes plus fixed state and replayed it
into the cache. Upstream's cache v2 does the same job from the recorded environments and keeps the
driver's own work, so the merge to upstream removed the key list. `learnings.md` keeps what it
taught.

## Files

| File | What it is |
|---|---|
| [`learnings.md`](./learnings.md) | Measurements and findings, oldest first. Read this before changing the design. |
| [`todo.json`](./todo.json) | The plan (TideTable): phases, statuses and a changelog per task. |

## What works today

- **Pipeline precompilation.** Pipelines build from the shader cache v2 log and the driver's
  pipeline binaries; `PrecacheShaders` builds the title's before play starts, on every CPU thread.
  The cache version is 54 and still reads 53 and 52, so an update keeps what the game has shown.
- **Static inventory.** `tools/nx2/shader_scan` extracts the shader programs inside a NUP container
  and writes a manifest. OoT's dump holds 39,392 unique NVN2 programs (36,176 BNSH plus BAHS, NDSR
  and NDVE); an offline sweep lowers 38,801 of them, 1,776 compile and pass `spirv-val`, 68 refuse
  to lower, and 521 need runtime context. `.bphsh`/`.bphsc`/`.bphnm` are Havok physics, not
  shaders.
- **Lowering audit.** `tools/nx2/lower_operand_audit.py` reports the opcodes the lowering handles
  but the register-pressure pass cannot yet shadow.

Static pre-lowering is not the delivery mechanism: lowering specialises on runtime uniform values
and storage pointer types, so recorded environments and the driver's own binaries, not a precompiled
shader archive, are what get shipped.

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
# {"phase":"native","implemented_opcodes":119,"control_blocks":91,"ir_instructions":12765,
#  "status":"blocked","error":{"code":"structure_failed",
#                              "message":"instruction 1575 (0x243): unsupported native opcode 0x243"}}
```

`--native-all` reports every blocked instruction in one pass instead of stopping at the first, which
is how the remaining work is measured. The IR from a tolerant build is for coverage only.

The report gives both answers: the opcode histogram says what is missing overall, and building the
program for real says at which instruction the frontend actually stops. `control_blocks`,
`syntax_nodes` and `ir_instructions` show how far the structured IR came out; `structure_failed`
with `unsupported native opcode` means the control flow was fine and an instruction is missing.

A program with an empty `unimplemented` list is fully covered. Opcodes are added one family at a
time, each with a captured instruction and its nvdisasm decode.

### Where it stands

Measured over all 4008 compute programs captured from Ocarina of Time: **3564 translate end to end**
(89%), and every one of those passes `spirv-val`. The rest are blocked by a few families.

| blocker | programs |
|---|---|
| surfaces (`SULD` 0xb97, `SUST` 0xb9b) | ~79 |
| `IADD3` / `IMAD` carry forms | ~80 |
| calls (`0x944`), `TLD4` (0xb63) | ~71 |
| shared-memory widths, `F2FP`, `R2P` and friends | ~90 |

- `shader-sweep <program> --native --output out.spv` compiles the native translation to SPIR-V.
  **All 3564 translating programs pass `spirv-val`.** The structurer emits the same block order the
  SPIR-V backend branches through: merge blocks inline after `EndIf`, returning selections for a
  conditional exit, and `Loop`/`Repeat` built from a back edge with a fresh header block.
- Loops are rare here (196 of 4008 programs) and none of those translate yet, so loop structuring
  has unit tests but no corpus coverage.
- Next: textures (`TLD`/`TXQ`, most of the remaining 893), then `EDEN_NX2_NATIVE=1` on a real save
  for a screenshot A/B.

## Live status

- A/B on Ocarina of Time (Kokiri Forest save) done through the debug server. `ALD`, `IPA` and
  `ISBERD`/`ISBEWR` translate now: 1,200 native attempts, 42 distinct graphics programs built
  natively, the image matching the lowering within temporal noise for compute and fragment.
- Still blocked for graphics: `AST` 0x322 (~402 fallbacks), `FRAG` 0x95b (~171), extended integer
  compares (~84), uniform shifts (~68), then `0xc17`, `0x20a`, `0xb63`.
- Mesh and vertex native still shade some meshes too dark, so `EDEN_NX2_NATIVE` covers compute and
  fragment; `EDEN_NX2_NATIVE_STAGES=mesh,vertex` opts in.
- Graphics output correctness: fragment color comes from `EmitFragmentExit` (R0+ mapped by the pixel
  shader header), the same rule the Maxwell frontend's EXIT applies, and `FMNMX`/`IMNMX` treat bit
  90 as a min/max swap rather than a negated predicate.
- The captured native stream is serialized with the environment in the pipeline cache (version 51),
  so the flag also works on cached programs, not just the first run.
- Next: those families, then the first end-to-end compile (`spirv-val`), then `EDEN_NX2_NATIVE=1` on
  a real save for a screenshot A/B.
- The `If` node a failed build leaves unfilled was a null-pointer trap for anything reading the
  syntax list; the structurer now types a reserved node immediately, and the sweep never
  dereferences a block it did not create.

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
| `EDEN_NX2_NATIVE=1` | Try the native frontend first; compute and fragment only by default. |
| `EDEN_NX2_NATIVE_STAGES=<names>` | Which stages `EDEN_NX2_NATIVE` may translate: `vertex`, `tess-control`, `tess-eval`, `geometry`, `fragment`, `compute`, `vertex-a`, `task`, `mesh`. |
| `EDEN_NX2_SPIRV_DUMP=<dir>` | Every emitted module as SPIR-V, plus each native program's captured stream as `<hash>_native.bin`. |
| `EDEN_NX2_PIPELINE_BUILD_TRACE=<file>` | One `BEGIN`/`END` line per driver pipeline build and a `NATIVE` line per native attempt. |
| `EDEN_NX2_SHADER_DUMP=<dir>` | Native programs with their header sidecars. |
| `EDEN_NX2_REJECT_HASH=<hash>,...` | Treat those programs as unsupported, for a reversible A/B. |
| `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` | Build every cached pipeline at boot. |
| `EDEN_NX2_PIPELINE_DIFF=1` | Log what splits a new pipeline from an earlier one that shares its shaders. |

## Open work

- 1.3.2 load title shader programs at boot for precache builds.
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
