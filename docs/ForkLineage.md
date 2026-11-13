# Fork lineage and upstream survey

Where this tree comes from, what each fork carries, and what of the others is worth taking.
Last surveyed 2026-11-11.

## The tree

```
Djihads80/Eglantine (nx2)          shared root, still the reference upstream
   |
   +-- zortos293/Lichii            continuation of Eglantine from #104, merged #567 again
          |
          +-- CompewterTutor/Lichii   this repository
          +-- Frizti/Fritchii         product fork, v5.8.x
```

| Repository | Created | Relationship | Carries |
|---|---|---|---|
| `Eglantine-Community/Eglantine` (was `Djihads80/Eglantine`) | 2026-10-29 | shared root ("VERY experimental Nintendo Switch 2 emulator") | the `nx2` branch every fork pulls from; lowering fixes, pipeline-cache and buffer-cache work |
| `zortos293/Lichii` | 2026-10-31 | continues Eglantine from `01423a2470` (#104) with the root commit `60ef6d2750` "COMMIT TO nx2 source", then merged Eglantine `nx2` up to `e736dfde9e` (#567, 2026-11-05) in `aba754a353` | shader cache v2, `shader_warm` precache, the live debug server, the frame dump, most Switch 2 game work |
| `CompewterTutor/Lichii` | 2026-11-05 | fork of `zortos293/Lichii`; this repository | the native NVN2 frontend (`src/shader_recompiler/frontend/native`), its tooling and dumps, input-stick and debug-server fixes |
| `Frizti/Fritchii` | 2026-11-05 | fork of `zortos293/Lichii`, described as a "Frontier GREEN HAT BOY ADVENTURE specialized Lichii fork" | its own product line: saves window, its own data folder, DLSS quality per game, NX archive support, ports of Eglantine experiment branches |

Since the shared base `e736dfde9e`, this tree has 459 commits and Eglantine's `nx2` has 250, of
which 84 touch `src/shader_recompiler` or `src/video_core/renderer_vulkan`. The two lines are
therefore siblings, not ancestor and descendant: neither is a superset of the other.

## Why Eglantine's commits arrive as ports, not as merges

Eglantine `nx2` has none of the subsystems this line was built on:

- no `src/video_core/shader_cache_v2` or `vk_shader_cache_v2` (no `ProgramRecord`; its
  `vk_pipeline_cache.cpp` is the older single-file cache),
- no `src/video_core/shader_warm`,
- no `src/shader_recompiler/frontend/native`.

A merge would resolve toward deleting our pipeline cache. Read each Eglantine commit, port what is
independent, and leave the structure alone.

The traffic is not one-way. Eglantine carries ports of Lichii work, for example `7602efc518`
"debugger, video_core: port Lichii's live debug server and frame dump", and `582ecc4568`
"shader: shadow ISBEWR data registers (Ocarina door NaN) (#100)" is a cherry-pick of this tree's
`b328477840`. When a Lichii change looks familiar, check whether it already went that way.

## Eglantine work not in this tree (shader and Vulkan, sampled)

The full list is `git log --no-merges HEAD..dj/nx2 -- src/shader_recompiler src/video_core/renderer_vulkan`.
Representative commits:

| Commit | Subject |
|---|---|
| `4ac9e703fe` | shader, video_core: bind storage buffers behind loaded pointers for OoT's vegetation |
| `c58d474d32` | shader: follow subgroup broadcasts when finding native storage buffer pointers |
| `900ab5a825` | shader: shadow copies for SHF, IADD3.UR, FSETP and I2F in the lowering |
| `993a31c593` | shader: gather each tap with its own offset when per-tap offsets stay dynamic |
| `9a69d8eaf0` | shader: lower Ocarina's byte uniform loads, ULEA carries, F2IP pairs and I2F.U8 of a UR byte |
| `1e2c2dd4db` | shader: lower BRA.U and HSETP2 with a constant buffer |
| `9a362357a2` | spirv: keep native global memory helpers as calls and route with one switch |
| `e88024e6b0` | shader, video_core: run Ocarina's end-of-frame network through a compact software HMMA |
| `0f9b6ab68a` | vulkan: keep lowered passthrough geometry shaders within device limits |
| `18106bdf49` | shader: split misaligned SSBO accesses, fix AMD depth sampling, CAS retries and shuffles |
| `eab2c2246d` | vk_pipeline_cache: keep the disk cache across translation-only version bumps |

The storage-buffer-behind-a-loaded-pointer work is the mature form of what the Lichii
`bisect/oot-*` branches started; take it from Eglantine rather than from those branches.

## Fritchii

Branches, as of the survey:

| Branch | Content |
|---|---|
| `nx2` | Lichii `nx2` plus one README commit; useful only as a pointer to the product line |
| `oot-gather-fix` | the v5.8.x product branch, 813 commits past Lichii `nx2` |
| `test/no-sky-fold` | Lichii `bisect/oot-6` (`a1f254cff6`) plus `36e8c6b7d4`, a test commit that undoes the IMul32 0/1 folding to see whether it causes OoT's flashes |
| `hotfix/v4.1` | older release line |

What `oot-gather-fix` adds is product-side, not rendering-side: an own data folder
(`%APPDATA%\fritchii`), a saves window that exports, imports and copies saves, per-game DLSS
quality and preset, NX archive support, and ports of Eglantine experiment branches (`EXP42`, `EXP47`,
`EXP48`, `EXP61`, `EXP62`, `EXP63`) plus extra register shadowing for SHF, IMAD and seven other
instruction forms.

Checked and not taken:

- the gathering lowering in `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h` is not
  something to port: where the two differ, this tree is newer (the `F2I`/`ULDC` operand rules), so a
  diff against Fritchii shows its line is stale there, not ours;
- `test/no-sky-fold` is the OoT bisect series plus one experiment; the finding it belongs to is
  covered by this tree's own bisect branches;
- the product features (saves, data folder, DLSS presets) are deliberate divergences, not fixes to
  fold in.

## Lichii branches this tree does not carry

`git for-each-ref refs/remotes/origin` lists about 125, mostly older than this tree.

- `bisect/oot-1` .. `bisect/oot-6`: the OoT bisect series, 17 to 27 commits each, contributing
  native storage buffers for graphics, pointer resolution for the vegetation shaders, the
  per-frame uniform-bank re-upload, and the IMul32 0/1 folding for the sky ambient. Eglantine now
  has the mature versions of the pointer work.
- `exp/oot-*`, `perf/oot-*`, `test/oot-*`, `port/ducky-oot-build16`, `fix/oot-*`: single-game
  experiments and performance branches, older than this tree and mostly folded into it already.

## Eglantine pull requests worth reading

Open at the time of the survey:

| PR | Subject |
|---|---|
| #811 | vulkan: port Orchid geometry passthrough fixes (pruned output generics, passthrough masks, DrawParameters) |
| #817 | vulkan: defer native shader emission to pipeline workers |
| #825 | nx2: async shader translation, compute pre-compilation, frame spike recorder |
| #814 | video_core: implement shader precompilation |
| #823 | fix shader warm precompile archives for FEFW |
| #762 | shader: expand native instruction lowering and backend support |
| #758 | nx2: port OoT Turnip matrix optimization series |
| #781 | shader: fold UISETP and USEL to IMNMX.U32 for indirect branch jumps |
| #721 | shader: keep depth-compare texture descriptors float on integer formats |

`gh pr diff <n> -R Djihads80/Eglantine` reads the change; read the diff, not the title.

## Re-running the survey

```sh
git fetch --no-tags https://github.com/Djihads80/Eglantine.git '+refs/heads/*:refs/remotes/dj/*'
git fetch --no-tags https://github.com/Frizti/Fritchii.git '+refs/heads/*:refs/remotes/fritchii/*'
git fetch origin '+refs/heads/*:refs/remotes/origin/*'

git rev-list --left-right --count HEAD...dj/nx2
git log --oneline --no-merges HEAD..dj/nx2 -- src/shader_recompiler src/video_core/renderer_vulkan
git log --oneline --no-merges HEAD..origin/bisect/oot-6
```

A commit that appears only on the other side of `HEAD...<ref>` is not in this tree; same subject with
a different hash usually means both lines took the same change independently.

## Fritchii is gone (2026-11-13)

`Frizti/Fritchii` returns 404 from the GitHub API and has no releases; only an unrelated
`Frizti/epic` repository remains under that account. The 2026-11-12 survey fetched it, and those
`refs/remotes/fritchii/*` commits are still in this checkout, so its history is not lost here, but
there is no upstream to follow any more. Stop watching it and do not treat its release notes as a
current source.

## Survey 2026-11-12

Facts that changed since the survey above.

- **Eglantine moved to `Eglantine-Community/Eglantine`.** `Djihads80/Eglantine` resolves to the same
  history and the same `nx2` tip (`6be10b5c12`), so the old URL still fetches; new work lands under
  the community organisation. Fetch the community repository and keep the old one only as a
  fallback.
- **This tree's parent is the real backlog, not Eglantine.** `zortos293/Lichii` `nx2` is at
  `3dd5c4e73c` with 93 commits this tree does not have, 46 of them under `src/shader_recompiler` or
  `src/video_core`. It is the line the shared shader cache v2 and the native frontend were built
  against, so these arrive as an ordinary merge rather than a port. The shader-side ones worth
  reading first: `7281d70993` (three precache fixes from nx2-experimental), `43af97ffaf` (reduce
  shader compilation work), `e84908679a` (SSA phis only when needed), `bd0171f7b9` (OoT light-table
  shadow maps through descriptor arrays), `4f3f9fe629` (Yoshi's 3D world: 3D SURED, bindless AOFFI
  gather, geometry ALD), `121e5c04ae` (fold UISETP/USEL to IMNMX.U32), `4c6ef2b810` (TLD4.B with a
  register handle, IMAD with a negated cbuf addend), `be24147966` (rebuild evicted graphics pipelines
  from kept SPIR-V) and `e63d0a04df` (keep graphics pipelines to a RAM limit, build all cached
  shaders at boot).
- **Eglantine itself has not moved** on `nx2` since the survey: still 250 commits past the shared
  base. It now publishes a `updater/tester-builds` branch and near-daily releases.
- **Fritchii is not the largest fork, it is the Ocarina of Time one.** 3.7k stars against Lichii's
  8.7k and Eglantine's 5.9k, Windows x64 only, last push 2026-11-11, release `fritchii-v5.8.4`.
  Its notes claim a 120k-shader precompilation pass, a frame pacer, built-in Dynamic FPS and shader
  preparation that only re-runs when the translation changes. Nothing of that is in this tree except
  the shared cache v2 lineage.
- **Smaller forks now exist and carry renderer work**: `MythosOfs/Orchid` (AMD and shader work,
  Mario Tennis first), `CheezerWheezer/CheezyEmu` (AMD FSR 4 and frame generation, branched from
  Lichii build 117), `sylvieistaken/selevet-eglantine` (PGO/ThinLTO tuning), `Franseven/PokeDinner`
  (Pokopia). Worth a skim only when they name a shader or renderer fix.
- **Reddit is not a source of technical detail.** `r/switch2emu` and `r/SwitchPirates2` carry
  gameplay clips, build announcements and shader-cache sharing threads; the recurring useful signal
  is that shared caches are frequently corrupt and that Fritchii is recommended for Ocarina on
  NVIDIA. Claims there need a commit to back them.

## The profile directory moved with the parent merge

`zortos293/Lichii` renamed the user directory in `src/common/fs/fs_paths.h` from `eden` to `lichii`
(`EDEN_DIR`). After moving this branch onto that line the emulator reads and writes
`%APPDATA%\lichii`, sees the old `%APPDATA%\eden` as a legacy profile, and offers the import prompt
that `UserDataMigrator` shows in `MainWindow` before anything boots. Until that prompt is answered
the build runs with no keys, no firmware and no saves, which looks exactly like a broken boot.

This tree's dev profile was moved by copying `eden` into `lichii` and marking the import resolved
with `%APPDATA%\lichii.migration\complete`. `eden` is left untouched, so the other forks keep
working. The save snapshot scripts and the live test runner now point at `lichii`.
