# Shader precompilation learnings

Findings from the switch2mac survey and the Ocarina of Time pipeline profile of 2026-11-06.
Numbers here come from two OoT sessions on the local machine (build `nx2-99ac3d5955`).

## 2026-11-06 - switch2mac shares pipeline identifiers, not shaders

- Source: [azmasm/switch2mac](https://github.com/azmasm/switch2mac) README, `tools/shader_list.py`,
  `games/<game>/shader_list.tsv`, and the nxnative engine notes it links.
- Learning: their "shared shader list" is one TSV per game and engine. A row is a short
  fingerprint of a shader or of a render setup (two shaders, colour formats, blending). It holds
  no code, paths, saves or machine details. They call it "library book numbers instead of books".
- Learning: the list is useful because the build **finds those fingerprints among the shaders of
  the player's own copy of the game** and prepares the matching pipelines before the first frame.
  A key list without a way to locate the shader code on the local machine is a work order that
  cannot be executed.
- Learning: collection is manual and per-title. The app keeps new rows in a contribution file,
  asks every 100 rows on exit, and the player attaches it to a GitHub form. A maintainer runs
  `shader_list.py collect` / `--apply --close` and commits.
- Learning: cost is real. Their Yoshi profile is about 133,000 shaders and "a full build can take
  many hours"; Star Fox is about 2,900 and a short build. Precompiling everything is a build-time
  job, not a title-screen job.
- Applies to: any Lichii precompile list; the same no-game-data rule and the same
  "identifiers only" format.
- Re-check when: their list format changes, or a game ships without locatable shader containers.

## 2026-11-06 - Lichii's pipeline cache already stores everything except the sharing layer

- Source: `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`,
  `src/video_core/shader_environment.*`, `src/video_core/pipeline_cache_files.h`.
- Learning: `<cache>/shader/<title>/vulkan.fork-v<N>.bin` is an append log of
  `[env count][serialized environments][pipeline key]` records. A graphics key is
  `6 x u64 shader content hash + FixedPipelineState` (1056 bytes); a compute key is
  `unique_hash + shared_memory_size + workgroup_size` (24 bytes). Driver blobs live separately in
  `vulkan_pipelines.fork-v<N>.bin`, and only when the driver does not cache pipelines itself
  (NVIDIA and AMD do).
- Learning: `vulkan_used.fork-v<N>.bin` records key hash -> sessions since use; boot rebuilds only
  keys used in the last two sessions (`MaxUsedPipelineAge`), or every cached key with
  `EDEN_NX2_PRELOAD_ALL_PIPELINES=1`. Without a used list, at most 3000 are built at boot.
- Learning: `GenericEnvironment::CalculateHash()` is `CityHash64` of the shader code bytes read
  from guest memory, so `unique_hash` is a content hash and is stable across sessions. That is
  the shareable half of a key.
- Learning: `FixedPipelineState` embeds `driver_id` and `driver_version`, and its layout depends
  on the device's dynamic-state feature set (`Size()` truncates at a different member for each
  feature mix). A shared list must therefore clear the driver fields and rows must be checked
  against the local feature set before use.
- Learning: environments are captured from live Maxwell registers while translating (cbuf values,
  texture types). They cannot be translated off-thread from live state; a background build needs
  a serialized snapshot (`GenericEnvironment::Serialize` / `FileEnvironment::Deserialize`), which
  is exactly what the boot loader uses.
- Applies to: any key list, prefetch or offline build; do not duplicate the cache format.
- Re-check when: `CACHE_VERSION` or `FixedPipelineState` changes.

## 2026-11-06 - OoT: cache churn, not a cold cache, is the constant stutter

- Source: two OoT sessions (`0400486024DEC000`), logs kept in
  `%APPDATA%/eden/log/eden_log.txt`; analysis of `ReportRuntimePipeline` and
  `EvictUnusedPipelines` lines.
- Learning: the cache works. First session booted with `Total Pipeline Count: 26`; the second
  booted with `1059` preloaded from the first session. The problem is what happens after boot.
- Learning: session 2 (11 minutes) built 1188 pipelines at run time and evicted 1206. At 169 s
  the first check after `EvictAfterFrames = 9000` (150 s at 60 fps) evicted **931 pipelines at
  once**, leaving 15. The game's working set is over 1000 pipelines revisited over minutes, so the
  idle-time policy mass-evicts them and the next area rebuilds them.
- Learning: 176 of 267 storm builds between 352 s and 361 s had already been built in session 1.
  That window built 612 pipelines in 10 s (1934 ms on the GPU thread) and its frames took 923,
  1041 and 1235 ms. About two thirds of the visible cost was rebuilding known pipelines, one
  third was genuinely new content.
- Learning: OoT generates a lot of programs: 509 unique shaders over 18 minutes (225 vertex,
  234 fragment, 21 geometry, 29 compute) and 347 distinct shader sets over 402 logged builds.
  55 builds shared a shader set with another key, so state variants do exist and are worth
  prefetching.
- Learning: `EDEN_NX2_PIPELINE_DIFF=1` reports which fixed-state fields split one shader set into
  several keys. It was off for these sessions, so the "0 differs" result is a disabled diagnostic,
  not evidence.
- Applies to: OoT and any title whose pipeline working set exceeds a few hundred.
- Re-check when: `EvictAfterFrames` or the boot preload policy changes, or on a faster/slower CPU.

## 2026-11-06 - Local debug setup on this machine

- Source: building and running OoT locally.
- Learning: this machine already has a Qt build at `build-qt/` (clang-cl from scoop, Ninja,
  Qt 6.11.1 from the CPM cache), a headless build at `build/`, and no `C:/lx` (that layout is the
  owner's PC). `cmake --build build-qt --parallel 12` is a no-op when up to date.
- Learning: user data is `%APPDATA%/eden`; the log is `%APPDATA%/eden/log/eden_log.txt`, the
  pipeline cache is `%APPDATA%/eden/cache/shader/<title>/` (not `%APPDATA%/eden/shader`).
- Learning: port 6545 is already held on this machine by a Stream Deck node plugin, so the debug
  server silently never listens. Launch with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6565` and query with
  `--server 127.0.0.1:6565`.
- Learning: the PowerShell wrapper here drops arguments that start with `{` when invoking Python,
  so `debug_mcp.py --call '{"method":"status"}'` arrives empty. Write the request to a JSON file
  and call `Adapter.run` from a small helper, or use git bash.
- Learning: `EDEN_DKB_FRAME_TRACE=1` gives per-second frame and pacing lines
  (`cpu_frame_wall_ms`, `fence_wall_ms`, `vblank pacing`, `DKB cpu` per-core timings) at
  `*:Info`, which is enough to correlate a spike with pipeline build timestamps.
- Applies to: any local runs and log analysis in this repo.
- Re-check when: the build layout or the debug server port changes.

## 2026-11-06 - Key list v1 works, and prefetch is not the biggest OoT win

- Source: the first key-list build (`vk_pipeline_keylist.*`, prefetch in `vk_pipeline_cache.cpp`)
  run on OoT over three sessions.
- Learning: the list round-trips. `keylist.bin` grew 1233 -> 1444 -> 1552 graphics rows (plus
  166 compute) across sessions with `0 rejected` on every load. Rows recorded at boot from the
  disk cache cover keys that were not built, so the list is a superset of a session's pipelines.
- Learning: prefetch fires and lands before the draw, but OoT gives it little work. About 9
  triggers and 22 sibling pipelines over four minutes, one to sixteen per trigger; every drained
  pipeline arrived in 10-290 ms. The list's 1552 rows form 924 distinct shader sets, and only 161
  of those have more than one state key (470 keys, largest set 36), so most OoT pipelines are
  unique shader content rather than state variants.
- Learning: prefetching mesh programs is not possible from an environment snapshot. The mesh
  registers are not in the serialized format, and trying produced repeated
  `Mesh output topology 0 ... is not implemented` errors. The fix is the boot loader's existing
  rule: skip keys whose environments are Task or Mesh, and remember failed keys so they are not
  retried.
- Learning: prefetched pipelines are appended to the versioned cache with
  `AppendPipelineRecord`, which writes the already-serialized environments and the key in the same
  record format `LoadPipelines` reads. The next boot loaded 1081 cached pipelines with no
  invalidation, so appended records parse correctly.
- Learning: frame spikes did not move much (39 frames over 33 ms, 6 over 100 ms, worst 901 ms at
  t=46 s, during the first scene). Eviction remains the dominant mid-game cost: 443 evicted in one
  check at 200 s and 175 evictions in a four-minute session. Phase 1.2 is the one that should
  change the stutter profile; the key list enables sharing and later static-shader prebuilds.
- Applies to: judging key-list work; do not expect it to fix a title whose pipelines are mostly
  unique shader sets.
- Re-check when: the eviction budget changes (1.2.1) or static shader discovery adds unreached
  content (1.3).

## 2026-11-06 - Resource compiler needs INCLUDE on this machine

- Source: rebuilding `lichii.exe` in `build-qt` after a CMakeLists change.
- Learning: `rc.exe` (SDK 10.0.26100) fails silently under Ninja when `INCLUDE` is unset: it cannot
  find `winresrc.h`, and `cmcldeps` reports only the banner. Set `INCLUDE` to the MSVC include plus
  the SDK `um`, `shared`, `ucrt`, `winrt` and `cppwinrt` directories before building.
- Learning: setting a partial `INCLUDE` (SDK only) breaks clang-cl's C++ standard headers
  (`'memory' file not found`), so include the MSVC STL path too:
  `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\include`.
- Learning: a running `lichii.exe` blocks the link (`permission denied`). Close the emulator before
  rebuilding; with `confirmStop=2` in `%APPDATA%/eden/config/qt-config.ini` a close no longer asks.
- Applies to: any local Windows build in this checkout.
- Re-check when: the VS or Windows SDK version changes, or CMake starts passing the SDK includes
  to the RC command.

## 2026-11-06 - Budget eviction removes the mass eviction; new content is what remains

- Source: `EvictUnusedPipelines` rewritten to an LRU budget (`EDEN_NX2_PIPELINE_BUDGET_MB`,
  default 1536 MB, about 0.45 MB per pipeline = ~3400 pipelines); `EDEN_NX2_PIPELINE_EVICT=legacy`
  restores the old rule. OoT run of 4.5 minutes on the same save.
- Learning: zero evictions in the session, versus 175 in four minutes and 1206 in eleven minutes
  with the idle-time rule. The 443-pipeline mass eviction at the boot screen is gone.
- Learning: run-time builds stayed similar (232 in 4.5 min versus 257 in 4 min before), so the
  eviction churn was not causing rebuilds in that particular stretch. The win is structural: a
  title revisiting an area now finds its pipelines still resident. The earlier storm evidence
  (176 of 267 builds were keys already built in session 1) is the case this prevents.
- Learning: the memory trade is real. The boot count was 1368 pipelines, roughly 600 MB of driver
  memory, and the working set can grow to the budget. `EDEN_NX2_PIPELINE_BUDGET_MB` is the knob.
- Learning: frame spikes did not disappear (57 frames over 33 ms, 9 over 100 ms in 4.5 min, worst
  822 ms at t=37 s during the first scene). Those are first-visit content and scene transitions,
  not eviction. That is the part a shared list plus static shader discovery (1.3) can move.
- Applies to: any title whose working set exceeds a few hundred pipelines; OoT and Mario Kart
  World are the known large ones.
- Re-check when: the per-pipeline memory estimate changes, or a title's working set exceeds the
  default budget.

## 2026-11-06 - OoT ships static shader containers; a zstd dictionary hides them

- Source: NUP TOC walk of the OoT dump (`0400486024DEC000`, update v131072) and byte sampling of
  the shader archives. Layout read from `src/core/file_sys/nup_container.cpp`.
- Learning: the romfs has 186,592 files, and 5,877 of them are
  `*.Nin_Ounce_NVN2.bphsh.zs` (60.4 MB total, per material, e.g.
  `Land_HrlField_ReductionCliff_A_01__Physical`), plus 1,772 `*.bphsc.zs` (596.3 MB, per area and
  stage, e.g. `MorphaStage_X0_Z0`), 846 `*.bphnm.zs` (per-area shader name tables) and 5
  `*.bfsha.zs` (43.8 MB, 3DS-style archives). So OoT does ship precompiled shaders.
- Learning: every one of those shader archives is a zstd frame with dictionary id 1. There is no
  zstd dictionary file in romfs or exefs, and no standard dictionary magic (`37 a4 30 ec`) in
  `main`, `sdk`, `rtld` or `subsdk0`. The engine almost certainly holds a raw-content dictionary
  with no header, so it cannot be found by magic or extracted by name.
- Learning: without that dictionary the archives cannot be decompressed with 7-Zip or Node zstd
  (`ZSTD_error_dictionary_wrong`). Extraction therefore needs one of: locate the raw dictionary
  inside `main` (reverse engineering), capture the decompressed archive from guest memory while
  the game loads the area, or reuse the game's own decompressor. The guest-memory capture is the
  cheapest option and still gives per-area static shaders before the first draw.
- Learning: this is the Zelda engine's own shader store, not a generic NVN2 container, so
  `tools/nx2/extract_static_shaders.py` (BNSH/CGSB/SHN/BND4/SARC) does not read `.bphsh`/
  `.bphsc` as-is. The audit script is `%TEMP%/nup_audit*.py` and only reads the NUP TOC.
- Applies to: phase 1.3.2 and any assumption that a shared key list can cover unreached content.
- Re-check when: the dictionary is recovered, or a debug build can dump `.bphsh` payloads from
  guest memory at area load.

## 2026-11-06 - Guest-memory shader capture works; target the raw header, not the lowered map

- Source: `scan_shaders` debug tool (`src/core/debugger/nx2_debug_tools.cpp`,
  `Tegra::MemoryManager::VisitMappedPages`), OoT in gameplay, plus a shader-dump session
  (`EDEN_NX2_SHADER_DUMP`, 1,020 graphics programs and headers).
- Learning: a Switch 2 title maps through the `Tegra::MemoryManager` its `nvhost_as_gpu` session
  creates, not Host1x's default `gmmu_manager`. A scan must walk
  `Tegra::MemoryManager::Instances()`; scanning the default one finds 0 pages. The active one had
  ~1.53 million mapped 4 KiB pages (about 6 GiB) and no big-page mappings, so the small-page walk
  is enough.
- Learning: `NEDENMAP` (the native address map) is appended by Lichii while lowering; it never
  appears in guest memory. Scanning for it returns 0. The raw program is a 128-byte native header
  followed by Ampere instructions.
- Learning: across 1,020 dumped headers the first word is one of `0x000C0481`, `0x020C0481`,
  `0x000CD482`, `0x000C5482`, `0x020C0C81`; bytes `0x08..0x18` and most of `0x30..0x7f` are
  zero. Searching the most common word over guest memory gave 256 hits and 84 windows; 6 of 200
  dumped program prefixes were found inside those windows, so loaded programs are discoverable.
  A false-positive rate needs measuring before this becomes a prebuild source.
- Learning: the `.bphsh` zstd dictionary is not present in guest memory during gameplay either
  (no `37a430ec` anywhere in ~6 GiB). The romfs route stays blocked unless the dictionary is
  recovered from `main` or the game's decompressor is reused.
- Applies to: phase 1.3.2. Next steps are validating header candidates by first-instruction
  decode, deriving the stage from the header, deduping by `NativeProgramKey`, and feeding the
  offline sweep.
- Re-check when: a stage-and-length-validated scanner exists.

## 2026-11-06 - `.bphsh` is Havok physics; the shaders are BNSH, BAHS, NDSR and NDVE

- Source: `scan_shaders` magic scan over OoT's application memory and every GPU address space,
  then a full TOC walk of the NUP.
- Learning: `.bphsh`/`.bphsc`/`.bphnm` are not shaders. A decompressed `.bphsh` starts
  `Phive` ... `SDKV20250200` and then `hknpMeshShape`, `hkcdSimdTree`, `hknpShape`: Havok
  physics shapes, scenes and navigation meshes. The 5,877/1,772 counts in the earlier note are
  physics assets and do not belong to phase 1.3.
- Learning: all of OoT's native shader code lives in five `.bfsha` archives (BNSH) plus the agl,
  gsys and tera `.sharcb` resources (BAHS), `agl_ndsr.pck`/`agl_ndsr_vr*.pck` (NDSR) and
  `Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs` (NDVE, 20 compute shaders for the AV1 movie upscaler).
- Learning: the zstd dictionaries are recoverable. Every `.zs` in the dump is a plain frame that
  names dictionary IDs 1-4 (24,117 + 9,542 + 160 + 8,381 frames; 58,902 more name none). All four
  are 128 KiB raw content, findable in guest application memory, and with those four every frame
  in the 186,592-file package decodes.
- Learning: a zstd frame's dictionary ID follows the window descriptor, not the header byte, and
  the frame content size sits after the ID. A bulk decompressor pre-allocating the declared size
  fails on the 23 MB SACR archives with `Allocation error`; a streaming decoder does not.
- Learning: a Nintendo SARC header is little-endian here and the data offset is at `+0x0c`
  (version at `+0x10`); reading `+0x08` yields the archive size and silently produces zero
  members. Members are raw, and a `.sharcb` starts with `BAHS`.
- Applies to: phase 1.3 and every later static inventory. `tools/nx2/shader_scan` implements it.
- Re-check when: another title's `.zs` frames name dictionary IDs outside 1-4.

## 2026-11-06 - OoT's static inventory is 39,392 NVN2 programs on this dump

- Source: `tools/nx2/shader_scan` (Rust, `cargo build --release`) over
  `The Legend of Zelda Ocarina of TIme [0400486024DEC000].nup`, key rule
  `sha256(control[:0x48] with +0x30 zeroed + code)[:16]`, deduped per stage.
- Learning: 186,592 files, 2,914 BNSH blobs, 132,771 program records, **39,392 unique
  `(stage, key)`**: 15,219 vertex, 16,862 fragment, 3,609 compute, 491 mesh, 17 task, plus the
  BAHS/NDSR/NDVE additions. By container the first pass had 36,176 from BNSH; the other 3,216 are
  2,443 fragment, 483 compute, 344 vertex, 9 mesh, 3 task.
- Learning: switch2mac's published `dm/games/0400486024DEC000/shader_map.tsv` has 42,855 rows and
  splits as BNSH 36,176 + BAHS 6,589 + NDSR 69 + NDVE 20. Our BNSH count matches exactly, so the
  BNSH reader is complete; the whole 3,463-key difference is 13 BAHS resources
  (`agl_technique_pfx` 2,039, `AglShader` 413, `agl_common` 292, ...) whose entry tables in this
  dump hold fewer shaders. Nothing we extract is absent from their map except 4 keys, so it is a
  content difference between the dumps, not a parser gap.
- Learning: `AglShader.sharcb`, `agl_common.sharcb`, `agl_technique_pfx.sharcb` and friends parse
  to 486 / 170 / 43 entries here; the same code reproduces 486 for `pfx` from this dump, so the
  reader matches theirs entry for entry. The check is `control[:4] == 34127698` and
  `u32(control+0x44) == len(code)`.
- Learning: `--extract` writes the whole set for the offline sweep: 37,948 unique code blobs
  (474 MB) and 16,616 control blobs (3.9 MB), plus a `manifest.json` in the
  `extract_static_shaders.py` schema, so `sweep_static_shaders.py` can lower and compile it
  without touching guest memory again.
- Applies to: phase 1.3.2. Next is the offline lowering/compile sweep and using the survivors as
  the build-at-boot list.
- Re-check when: the base v0 dump is available, which is the likely source of the 3,463 extra
  BAHS shaders.

## 2026-11-06 - Offline sweep: 98.5% lower, but graphics builds need runtime state

- Source: `tools/nx2/shader_scan --extract` (39,392 programs, 474 MB of code), then
  `tools/nx2/sweep_static_shaders.py` with `build-sweep/bin/shader-sweep.exe` (built with
  `ENABLE_DEBUG_TOOLS=ON`), the owner's `vulkan.fork-v50.bin` as the context cache, and
  `spirv-val` 2025.5.
- Learning: 38,801 of 39,392 programs lower (98.5%). 1,776 compile and pass `spirv-val` with
  synthetic pipeline defaults (1,581 vertex, 195 fragment). The other 39,322 stop at `context`.
- Learning: the blocker is state, not code. The two dominating refusals are
  `Uncaptured constant buffer value at 0x100000000` and
  `Uncaptured storage pointer classification at 0x1b0`: Lichii's lowering specializes on runtime
  uniform contents and storage pointer types. A static archive alone therefore cannot build a
  graphics pipeline; it can only pre-translate. Full pipeline builds must come from the key
  list's recorded environments.
- Learning: 68 programs refuse to lower at any context. By native opcode: `0xabb` 20 (uniform
  constant load mode), `0xb63` 20 (per-tap gather), `0x944` 8 (call linkage mode), `0xc08` 4,
  `0x905` 4, `0x3a9` 2, `0xb66` 2, `0x243` 2, `0x981` 2, `0x217` 1, `0x3c2` 1, `0x899` 1,
  `0x481` 1. Sources: `FoliageShader.sharcb` 30, `agl_ndsr_vr.pck` 14, `agl_common.sharcb` 13,
  `agl_technique_pfx.sharcb` 5, `tera_develop.sharcb` 2, `TerrainGround` bfsha 2. These programs
  would also fail in game, so they are real lowering gaps rather than extraction noise.
- Learning: one NDVE compute program carries 1,066,752 bytes of code (66,672 instructions), over
  the sweep's 65,536-instruction cap. The AV1 upscaler's network is fully unrolled, so the cap
  needs raising before that one can be swept.
- Learning: the sweep needs `spirv-val` and a `shader-sweep` build; on this machine MSYS2
  `mingw-w64-x86_64-spirv-tools` provides the validator and `build-sweep` is configured with
  `-DENABLE_DEBUG_TOOLS=ON -DENABLE_QT=OFF` plus the MSVC/SDK `INCLUDE` and `ml64` on `PATH`.
- Applies to: phase 1.3.2. Choose between pre-lowering everything into a native-program-keyed
  cache, or treating key-list environments as the only pipeline source.
- Re-check when: lowering stops specializing on constant buffer contents, or a runtime-state
  synthesizer exists.

## 2026-11-06 - Key-list tooling, and what actually splits OoT's pipelines

- Source: `tools/nx2/keylist_tool.py` (inspect, merge, programs, diff) over the OoT key list in
  `%APPDATA%/eden/cache/shader/0400486024dec000/keylist.bin`, written by the budget-eviction
  session.
- Learning: the list had 1,778 graphics and 195 compute rows, 1,805 distinct shader programs and
  1,164 distinct shader sets: 858 of the states sit on sets that already had another state, and one
  3-stage set carries 36 states. The format is `lchkeyls` v1 and rows are self-describing, so the
  tooling is a plain file reader (`tools/nx2/keylist_tool.py`).
- Learning: measured `FixedPipelineState` offsets for the stored 0x70-byte prefix are 0x00 raw1
  flags, 0x04 raw2 (depth/alpha flags), 0x08 color formats, 0x10 driver id/version (always zero in
  the file), 0x18 alpha ref, 0x1c point size, 0x20 viewport swizzles, 0x40 `attribute_types`,
  0x48 dynamic state, 0x50 blend attachments. `Size()` picks the prefix from the dynamic-state
  features; OoT's rows all store 0x70 bytes, the `dynamic_vertex_input` branch that ends before
  `attributes`, and transform feedback stores the whole struct.
- Learning: the state bytes that split one shader set are dominated by `attribute_types`
  (389/339/295/267 states across the top sets), then raw2 depth/alpha flags (260/259), color
  formats (259), dynamic state (167) and blend attachments (25). That is real variation, not noise:
  `attribute_types` feeds `Shader::AttributeType` in `MakeRuntimeInfo`, so integer versus float
  vertex inputs compile different shaders and cannot be dropped from the key.
- Learning: `driver_id`/`driver_version` are zero in every stored row, so lists written here are
  portable as intended.
- Applies to: phase 1.4. A shared list is only worth shipping if the rows are states the game
  really drew, which the diff output now makes checkable before export.
- Re-check when: the dynamic-state surface changes (a new `Size()` branch) or `FixedPipelineState`
  gains fields inside the prefix.

## 2026-11-06 - The OoT `nup` was never corrupt; my launch path was wrong

- Symptom: every launch ended at `core.cpp:403: Failed to obtain loader`, with
  `AppLoader_NUP: NUP container is a standalone update; booting the update` logged twice just
  before. The GUI said "The ROM format is not supported".
- Cause: `GetLoader` received a null file. Instrumenting it showed `file=<null>` on the two calls
  that follow the frontend's identification passes. `GetGameFileFromPath` returns null when
  `FS::IsFile` says the path is not a file, and the path I passed had one digit too few:
  `...Ocarina of TIme [0400486024DEC000].nup` while the file on disk is
  `...Ocarina of TIme [04004860224DEC000].nup`. The size and mtime never changed, which is why the
  file itself was never the problem.
- Learning: the NUP loader branch in `GetLoader` had never been reached in those runs; the two
  `standalone update` lines come from the NUP constructor invoked by `IdentifyFile`, which proves
  the container parses even when the boot path gets a null file.
- Learning: `core.cpp` now logs `No file at <path>: check the path, it may differ in case or digits`
  and returns `ErrorGetLoader` instead of the generic "failed to obtain loader" after a null file.
- Learning: a base-commit rebuild from scratch failed with a truncated `1 error generated` and a
  `build-base` binary that still reported the feature branch, so a build-based bisect was telling
  us nothing. Read the path the log actually printed before bisecting code.
- Applies to: any "ROM is broken" report. `Get-Item` size and mtime first, then the exact path
  string, then the loader.

## 2026-11-06 - Debug-server control surface verified live on OoT

- `tools/nx2/live_check.py --server 127.0.0.1:6565` now runs the whole phase 1.6 surface against a
  booted game; 21 checks, all passing on `nx2-9cecb6c961` with OoT at the title screen.
- Measured: pause froze the present counter exactly; `step {frames:10}` advanced exactly 10
  presents; input_sequence, input_record and input_playback worked; guest read at `0x8000000`;
  GPU map reported ranges starting at `0x11471b3000`, bulk read, write-back and re-read matched;
  `shaders` reported 57 programs with 0 rejected; `status.caches` matched the pipeline cache
  (195 compute, 621 graphics, 1778 key-list graphics keys, 0 evicted).
- Fixed while verifying: playback parsed the recorded `buttons` hex text with the button-name
  parser, so every replay threw `unknown button "0000000000000001"`. Recordings round-trip through
  `ParseRecordedButtons` now, with a unit test.
- Fixed while verifying: `input_playback` reported `events: 0` because it read `steps.size()` after
  the vector had been moved into the playback thread.
- Fixed while verifying: there was no way to discover a mapped GPU address, so every
  `read_gpu_memory` failed with "is not fully mapped". `MemoryManager::FirstMappedRanges` walks the
  page tables (word-skipping, big pages first) and the new `gpu_memory_map` tool reports the
  ranges. Read-only, under the manager's guard.
- Learning: two playback runs of the same macro advanced the same 73 presents each time, which is
  the first evidence that record/replay is deterministic enough for A/B work.
- Learning: the screenshot shader toggle is reversible in one call: the response lists the
  override, the image differs, and `status.rejected` is empty afterwards.
- Applies to: phases 1.6.1, 1.6.2, 1.6.3 and the verified part of 1.6.5 and 1.6.6.
- Re-check when: motion or trigger input is exposed, or render overrides are added to screenshots.

## 2026-11-06 - A session contributes only the rows a shared list is missing

- Problem: `Save` rewrote `keylist.bin` with every known row, so two players collecting the same
  title could only overwrite each other's file. Sharing needs a contribution, not a replacement.
- Design: `PipelineKeyList` remembers which hashes each `Load` brought in. `SaveContribution`
  writes the rows whose hash is in no loaded list, in the same format, sorted by key hash.
  `LoadContribution` reads a pending file back without marking its rows as known, so an unmerged
  contribution survives a later session that adds nothing; when nothing is pending the file is
  removed instead of left stale.
- Measured on OoT with the debug server: a boot with no list (list parked) recorded 1504 graphics
  rows, and after loading the save 1583 graphics + 195 compute, with
  `status.caches.keylist_contribution` equal to the row count. With the merged 1778-row list in
  place, the same session reported `keylist_contribution: 0` and wrote no contribution file.
- Measured merging: `keylist_tool.py merge` of the parked 1778-row list with that contribution
  added 0 rows and produced a byte-identical 319,960-byte file, and merging twice stayed
  identical, so the union is idempotent in practice and not only in the tests.
- Learning: the key list lives under `cache/shader/<title id>/` (shader cache), not under a
  `shader` folder at the data root, and it is written either on a 2-minute-used-pipeline trigger
  or by the destructor, so a forced process kill loses the session's rows.
- Applies to: phase 1.4.1, and it is the input 1.4.2 needs for a shared list.
- Re-check when: a fetched list gets its own directory, since the contribution then has to name the
  list it is missing rows from.

## 2026-11-06 - Indexed uniform loads carry subword sizes; the sweep fails one gap at a time

- Gap closed: `0xabb` (ULDC indexed) refused width 0 and 2 because `subword_load` was written for
  the direct form only. Maxwell's `LDC` decodes U8/U16 in every mode, and the indexed emit path
  already passed the width and the index register through, so the fix is to accept both widths for
  both forms (still with the alignment, bank, signed-offset and mode-word checks). All 20 OoT
  programs that failed here were compute shaders with width 0 or 2.
- Result: 4 of those 20 now lower completely; the other 16 fail at the *next* instruction. The
  realignment with the debug server shows the world renders correctly at Kokiri Forest, so the
  change does not disturb the shaders that already worked.
- Learning: a program holds several gaps. The sweep reports only its first failure, so a fix that
  makes 20 programs "pass this check" mostly moves them to a new error. Re-check the affected
  programs after each change with `shader-sweep --lower-only` rather than trusting the first error
  message.
- Remaining chain, as of this pass (68 programs total, 4 now lower):
  | Programs | Message | Instruction |
  |---:|---|---|
  | 14 | per-tap gather control-flow proof | `0xb63` |
  | 10 | LEA carry/high mode | `0x291` uniform LEA whose bits 81-83 carry-out is 2 |
  | 8 | call linkage mode | `0x944` |
  | 4 | unsupported native opcode | `0x905` fragment, `00017905 7fffffff 0021f000` |
  | 4 | unsupported native opcode | `0xc08` fragment, `01017c08 00000004 08800000` |
  | 4 | packed float-to-byte mode | `0x243` |
  | 3 + 3 | per-tap gather operands / offset proof | `0xb63` |
  | 2 | unsupported native opcode | `0x3a9` compute, `000073a9 00000008 001ee109` |
  | 2 | register shadowing mode | `0x217` |
  | 2 | texel fetch mode or descriptor binding | `0xb66` |
  | 2 | global load uniform addressing mode | `0x981` |
  | 2 | byte-to-float conversion mode | `0x243`-family |
  | 2 | uniform constant load mode | `0xabb` width 0, offset `0xffff` |
  | 1 | unsupported native opcode | `0x481` compute, `020c0481 00000000 00000000` |
  | 1 | uniform shift mode | `0x899` |
  | 1 | register shadowing mode | `0x3c2` vertex |
- Open question for the two remaining ULDC failures: offset `0xffff` on a subword load is either
  -1 byte (fits Maxwell's signed 16-bit LDC offset) or +65535. The current guard refuses it as
  unrepresentable, and the Ampere field alone does not settle it; a decode with nvdisasm on the
  captured instruction, or a visual A/B of the affected compute shaders, would.
- Applies to: phase 1.3.3, which is now in progress rather than a single change.

## 2026-11-06 - Register shadows need an operand list per opcode, and OoT names several that were missing

- Symptom: `native register shadowing mode`. When a native instruction names a guest register at or
  above the lowering's high-uniform base, `PrepareRegisterPressure` re-expands it with low shadow
  copies, but only for opcodes it knows: everything else hits the `default:` branch and the whole
  program is rejected.
- Fixed: added operand lists for `0x811` (LEA with an immediate addend, which lives in words[1] and
  is not a register), `0xc20` (FMUL with a uniform source), `0x217`/`0xa17` (IMNMX register and
  cbuf forms), `0x219`/`0xa19`/`0xc19` (register, constant and uniform shifts), `0xc23` (FFMA with
  a uniform addend) and `0x3c2` (R2UR, whose destination is a uniform register, so only bits 24-31
  is a shadow candidate).
- Learning: the pass needs the same operand knowledge the lowering has, and the two had drifted.
  Each of these was found by decoding the failing instruction from the sweep, asking which bit
  fields the lowering reads as guest registers, and listing only those; listing a uniform register
  index or an immediate field as a GPR would reserve the wrong registers.
- Result: 2 more of the 68 OoT programs lower completely (6 now), and the two programs that failed
  here walk past several such instructions, so one opcode per program was not enough: expect the
  remaining gaps to be reached only after several rounds.
- Test: `src/tests/video_core/ampere_register_pressure.cpp` checks the captured LEA, FMUL, FFMA and
  shift instructions plus synthesised IMNMX/R2UR cases, and that an unknown opcode still refuses.

## 2026-11-06 - The pressure pass cannot be made total by guessing; the operand lists must be filled in

- Measured the drift: `lower_to_maxwell.h` handles 234 native opcodes, `register_pressure.h` lists
  operands for 94. The other 140 reject any program that needs a shadow for one of them. That is the
  whole "native register shadowing mode" class, and it is why the OoT sweep keeps finding them one
  game program at a time. `tools/nx2/lower_operand_audit.py` prints the difference on demand.
- Tried the obvious shortcut: an unknown opcode falls back to the convention the lowering already
  uses for "does this instruction touch a high register" (fields 16, 24 and 64 always, 32 only when
  bits 9-11 say it is a register). It lowers correctly for the OoT cases, but it breaks deliberate
  guards that the test suite already encodes: `Fast Fusion` and `Yoshi` assert that an opcode
  without an operand list is rejected even when the borrow would fit. The reason is sound: a wrong
  operand list rewrites a field that is not a register and silently corrupts the program, while a
  rejection costs one skipped draw and a log line. Reverted.
- Learning: the lowering's own high-register test is not a safe source for the rewrite set. It only
  decides whether a copy is needed; it never has to know that a field is a register rather than an
  immediate, because an immediate that happens to be at or above the base only triggers an extra
  (harmless) copy decision. Rewriting needs the precise answer.
- Learning: case groups in the lowering are not uniform either. `0x211`/`0x811`/`0xa11`/`0xc11`
  share one body, but `0x211` reads three registers and `0xa11`/`0xc11` read a constant-buffer or
  uniform operand instead, which is why the pressure pass lists them differently. Each opcode has
  to be judged on its own branch inside the shared body.
- Method for the remaining audit: for each missing opcode, read its branch (not its label group),
  list the fields that branch reads as guest registers, and where the body uses the `operand(i)`
  helper resolve it through that helper's `form = Bits(9, 3)` mapping (which=0 is bits 24; 1 and 2
  are bits 32 or 64 depending on the form). Validate a batch with the sweep over the known-failing
  OoT programs and the whole `[video_core]` suite, which is what catches a wrong list.

## 2026-11-07 - Which pressure entries are worth adding, and which cannot matter

- Two batches filled ten opcodes (FADD/FMNMX immediate, cbuf and uniform forms, F2F.F16.F32,
  IDP.2A.LO, ULDC direct and indexed), taking the audit from 140 to 130. Neither batch moved an OoT
  program: none of the 68 reach the shadow stage on those instructions. The entries remove latent
  rejections, not current ones.
- Rule that saves work: an opcode whose operand fields hold uniform-register indices or predicate
  slots can never need a shadow, because those values are below 64 and the reserved base starts at
  216. `0x882`, `0x883`, `0x886`, `0x88c`, `0x28c`, `0x89c`, `0x81c`, `0xc82` and `0x2a5` are all in
  that group, so leaving them out of the pressure switch changes nothing.
- Rule that makes a sibling cheap: when a family branch differs only in where source B comes from
  (register, immediate, cbuf, uniform), it can reuse the register form's list, because the
  non-register forms keep a value below the reserved base in that field and the pass only rewrites
  fields that land in a mapped high quartet.
- Limit found: multi-tap gather (`0xb63`/`0xf63`) cannot take a flat list. Its destination span is
  `popcount(mask)` and may split across bits 16 and 64, source A spans by dimension, and source B
  spans 2 registers for packed per-tap offsets. Under-listing corrupts silently; over-listing just
  rejects again. It needs the lowering's own expressions mirrored into the pass, so it is the next
  real unit of work rather than a batch edit.
- Applies to: 1.3.4. `tools/nx2/lower_operand_audit.py` is the scoreboard.

## 2026-11-07 - TLD4 gather under a shadow is a guard, not a missing table entry

- Tried the gather entry (`0xb63`/`0xf63`) with the spans mirrored from the lowering: destination
  `popcount(mask)` split across bits 16 and 64, coordinates a pair, source B a pair for per-tap
  packed offsets. The register-pressure mechanics accepted it and the lowering proceeded.
- `ampere_yoshi_title.cpp:279` then failed. That test asserts a uniform array gather with its
  destination at the reserved base is *rejected* under a small reserve (`LowerToMaxwellWith(code,
  4)`), and it failed because the entry made the shadow succeed rather than because the spans were
  wrong: with `high_uniforms = 4` the reserve is 212, the destination quartet 212-215 maps to a free
  low quartet, and the instruction lowers.
- Reverted. Whether a gather is correct when its handle, coordinates and packed per-tap offsets are
  copied into shadows is not something the operand table decides: the per-tap path also scans the
  preceding instructions in the unshadowed `code` to prove the offsets are constants, so mixing a
  shadowed gather with unshadowed producers needs its own analysis and its own test.
- Learning: the pressure table is not the only gate on these opcodes. Adding an entry can turn a
  deliberate "unsupported" into a wrong lower, which is exactly why the existing tests assert the
  rejection. Check what else guards an opcode before listing it.
- Applies to: 1.3.4. The gather group stays out of the table until shadowed gathers are proven.

## 2026-11-07 - The undecoded opcodes, decoded with nvdisasm on this machine

- The CUDA toolkit here has `nvdisasm` (12.4 through 13.3), and `nvdisasm -b SM90 file.bin` decodes
  a raw instruction binary, so no shipping of game data or CUDA project is needed: write the four
  little-endian words of one instruction to a file and disassemble it. A word nvdisasm refuses stops
  the run, so try one instruction per file and several architectures.
- Decoded, with the captured words after the opcode:

  | Opcode | Words | Disassembly (SM90) | Lowering gap |
  |---|---|---|---|
  | `0x291` | `060a7291 0000000a 0f84103f 000fe200` | `ULEA UR10, UP2, UR6, UR10, 0x2` | bits 81-83 name the *uniform predicate that receives the carry out* (UP2), not a plain flag. The rewrite path rejects a uniform LEA with a carry out because it would have to write R239 bit 6-2. |
  | `0x905` | `00017905 7fffffff 0021f000 000e2200` | `F2I.FTZ.U32.TRUNC.NTZ R1, +QNAN` | Float-to-unsigned-int with an *immediate* source; only the register form (`0x311`, U64) exists. |
  | `0xc08` | `01017c08 00000004 08800000 000fe400` | `FSEL R1, R1, UR4, P1` | Float select with a uniform second operand. |
  | `0x3a9` | `000073a9 00000008 001ee109 000ea400` | `ATOMG.E.CAS.STRONG.GPU PT, R0, [R0], R8, R9` | Global compare-and-swap; only `0x3a8` (atomic add/min/max and strong GPU U32) is lowered. |
  | `0x481` | `020c0481 00000000 00000000 00000000` | none on SM87/90/100/101/103/120/121: "Unrecognized operation for functional unit 'uC'" | Either an NVN2 encoding this toolkit does not know, or the sweep picked up data rather than an instruction. Check the containing program before treating it as a missing opcode. |

- Learning: the decode wall is gone, and it settles questions the byte patterns could not. For
  `0x291` the field is a predicate destination, which explains why the existing guard refuses it: a
  rewrite has to materialise the carry into R239 with the same machinery `ULOP3` uses.
- Applies to: 1.3.3's four "unsupported native opcode" groups, and the LEA carry group.

## 2026-11-07 - FSEL with a uniform source: 6 to 10 of the 68 lower

- `0xc08` decoded as `FSEL Rd, Ra, URb, Pp` and is the uniform-source sibling of the bitwise
  SEL/FSEL case (`0x208`/`0x808`/`0xa08`). The lowering needed one extra branch: bits 32-37 hold the
  uniform index, bits 87-90 the select predicate and its `.NOT`, and bit 91 marks the uniform form.
  The uniform index maps through `uniform_register`, so UR4 becomes R220 and the same Maxwell SEL
  encoding is emitted.
- Two details the first attempt got wrong, both visible only because the captured programs were
  re-run: the allowed word mask must include the predicate and `.NOT` bits (`~0x0f800000`, not
  `~0x08800000`), and source `RZ` (255) is a legal first operand, so only the uniform index may be
  rejected.
- Result: 10 of the 68 OoT programs now lower, up from 6. The four that changed are fragment
  shaders whose only remaining gap was this select.
- Learning: with nvdisasm available locally the loop is short: decode the word, find the sibling
  case the lowering already has, add the one field the sibling does not cover, and re-run the
  captured programs. Each of the four "unsupported native opcode" groups has such a sibling except
  `0x3a9` (global CAS).

## 2026-11-07 - Native frontend scaffold, and the first coverage number

- Added `frontend/native/` with a coverage query (`IsImplemented`, `ImplementedCount`, `Missing`) and
  `shader-sweep --native`, which reports per program the opcodes with no direct IR translation yet.
  The scaffold deliberately implements only NOP and EXIT until an arithmetic family lands, so the
  number stays honest.
- First measurement, one captured OoT compute program (1920 instructions): 100 distinct opcodes and
  1907 instructions unimplemented. The top four are FFMA `0x223` (157), FADD `0x221` (155), FMUL
  `0x220` (143) and `0xf89` (124): 579 instructions, 30% of the program, from four opcodes.
- Learning: a native frontend is a large but measurable port, and the corpus makes it a scoreboard
  rather than a leap. Four arithmetic opcodes are worth 30% of a program; the long tail is the
  remaining 96 opcodes at a handful of uses each.
- Learning: `0x000` and `0x00c` appear as opcodes in the raw allocation (23 and 1 instructions in
  that program). They are padding past the program's end, so the native frontend must ignore zero
  words rather than treat them as unimplemented opcodes, otherwise coverage looks worse than it is.
- Applies to: phase 1.7. The lowering stays the default and the oracle until 1.7.6.

## 2026-11-07 - Native frontend 1.7.2: the arithmetic families emit IR

- `frontend/native/decode.h` decodes an ALU source into register / immediate / constant buffer /
  uniform, with the absolute and negate modifiers, using the same field layout the lowering already
  trusts. Field values confirmed against captured words, not guessed: `110f7a20 00c13500 ...`
  decodes with nvdisasm as `FMUL.FTZ R15, R17, c[0x3][0x4d4]`, and the decoder reads bank 3 at
  bits 54-58, byte offset 0x4d4 at bits 38-53, destination R15 at bits 16-23, first source R17 at
  bits 24-31 and scale 4 at bits 84-86.
- `frontend/native/translate.cpp` emits IR for FMUL, FADD, FFMA (register, immediate and constant
  buffer forms) and the immediate-lane SHFL. The floating control word maps 1:1 onto the Maxwell one
  the lowering already produces (bits 78-79 rounding, bit 80 flush-to-zero, bit 77 saturate), so both
  frontends agree on the FP flags for the same instruction.
- Instruction predicates are real in this corpus: 138 of 155 FADDs in the captured compute program
  are guarded (`@P4 FADD.FTZ R16, R16, R39`). The IR has no per-instruction predicate, so a guarded
  destination becomes `Select(guard, value, old value)`, which is exact for a register write. The
  Maxwell frontend ignores these guards, so this is one place where the native path is more correct
  than the lowering rather than merely equal.
- Uniform-source forms (6 and 7) are routed to a precise `native uniform source` failure instead of
  being absent from the dispatch, so the coverage report says why a program stops. They become SSA
  values in 1.7.3.
- Scoreboard: the captured OoT compute program goes from 1884 to 970 unimplemented instructions
  (91 distinct opcodes left) with 15 implemented opcodes. The translation still stops immediately:
  the program opens with `0x919`, which is not an arithmetic opcode, so whole-program coverage needs
  the control-flow and system-opcode work that 1.7.5 and 1.7.6 carry.
- Learning: the decode helper is the piece worth keeping shared. Every field the native frontend
  needs is already written down in the lowering, so the port's cost is IR semantics, not bit layout.

## 2026-11-07 - Native frontend 1.7.3: uniform registers and predicates as SSA values

- UR0-UR63 live in their own IR register bank (`UNIFORM_REGISTER_BASE = 512`) and are read with a
  plain `GetRegister`. Nothing is remapped into R216-R245, no copy is inserted, and the pressure
  table is not consulted, because the native path has no second register file competing for the 256
  general registers.
- IR has no uniform predicate file. UP0-UP6 are ordinary registers holding the hardware's own
  representation (0 or all ones, the bits R239 carries on Maxwell), and `UniformPredicate` turns that
  mask into an `INotEqual(..., 0)` condition when a predicate value is needed.
- PLOP3.LUT (`0x81c`) becomes seven `Select` instructions over the eight truth-table bits. The LUT
  index convention is `a | b << 1 | c << 2` with operands in assembly order, confirmed against the
  captured copy idiom `PLOP3.LUT P0, PT, PT, PT, UP0, 0x80, 0x0`: only table entry 7 is set, so the
  result is PT & PT & UP0 = UP0. The three source fields are bits 87-90, 77-80 and 68-71, and bit 67
  makes the third source a uniform predicate — the same fields the lowering reads.
- Uniform instructions guard with a uniform predicate rather than a per-lane one (UMOV, and the
  uniform-predication list the lowering keeps), so their guard is read from the UP bank.
- Measurements on the captured OoT compute program: 970 to 813 unimplemented instructions, 82
  distinct opcodes, 27 opcodes implemented. The uniform group accounted for 94 instructions
  (`0x81c` 50, `0xc20` 20, `0xc02` 17, `0xc82` 5, `0x882` 1, `0xe23` 1); MOV added 63 more.
- Correction to the 1.7.3 plan: no program in the corpus fails the lowering with "native register
  shadowing mode", so the original success criterion ("a captured program using UR28+ lowers without
  shadow copies") cannot be demonstrated by a lowering failure disappearing. The property holds by
  construction instead — there is no reserved base to remap into — and the progress shows up as the
  uniform opcodes above translating.
- Whole-program translation still stops at instruction 0: the program opens with `0x919`, which
  nvdisasm decodes as `S2R R2, SR_TID.X`. System opcodes and control flow are the next real gate,
  not arithmetic.

## 2026-11-07 - Native frontend: S2R and the set-predicate family

- `0x919` decodes as `S2R Rd, SR` with the special register number in the high byte of word 2 and
  nothing else in word 2, which is a strong self-check: the encoding is rejected unless
  `words[2] == special << 8`. `0x9c3` is the uniform variant (S2UR). The specials implemented mirror
  what the Maxwell S2R already reads, plus `SR_VARIABLE_RATE` (132), which is the 1x1 fragment size
  `0x00010100`.
- `0x20b`/`0x80b`/`0xa0b`/`0xc0b` (FSETP) and `0x20c`/`0x80c`/`0xa0c`/`0xc0c` (ISETP) map onto the
  Maxwell translator's own building blocks: bit 81-83 destination, bit 84-86 second destination,
  bit 87-89 the accumulated predicate with its negation in bit 90, bit 74-75 the combination
  (AND/OR/XOR), bit 76 the compare operation and bit 73 the integer signedness. Reusing
  `IntegerCompare`, `FloatingPointCompare` and `PredicateCombine` means the semantics are the same
  code the Maxwell path runs, not a second implementation of them.
- Verified against captured words before writing any test: `2e00720b ... 03f14000` gives compare 4 =
  GT with bit 80 set, which nvdisasm prints as `FSETP.GT.FTZ.AND P0, PT, R46, RZ, PT`; `3200720c ...
  03f46070` gives compare 6 = GE with bit 73 clear, printed as `ISETP.GE.U32.AND P2, PT, R50, R4, PT`.
- Scoreboard: 813 to 680 unimplemented instructions, 76 distinct opcodes, 37 implemented. The
  translation now gets past the program start for the first time: it stops at index 2, `0x824`
  (`IMAD.U32 R5, R3, 0x60, R2`).
- Learning: the remaining top of the list is finally the structural work rather than an opcode gap —
  `0x947 BRA` (53), `0x812 LOP3` (51), `0x981 LDG` (39), `0x308 MUFU` (38), `0x803 P2R` (31). Branches
  need the block graph (`TranslateBlock` is still one linear block), memory needs address analysis.

## 2026-11-07 - Native control flow: the shapes NVN2 actually emits

- The control-flow instruction set matches Maxwell's model: BSSY Bn, target names a reconvergence
  point, BSYNC Bn closes the region, BRA leaves it, EXIT ends the program. BREAK and WARPSYNC are
  reconvergence management with no host effect, so both the structurer and the translator drop them.
- Measured shapes in the captured OoT compute program (1920 instructions): 53 BRA, 14 BSSY, 14 BSYNC,
  4 EXIT, 52 forward branches, exactly 1 backward branch — and that one is the closing self-branch of
  a computed jump. The program has no loops at all, only if/else diamonds, so loop structuring is not
  needed to cover it.
- Branch encoding, taken from the lowering's own relocation maths and unchanged: the target is
  `index * 16 + 16 + sign_extend_50(words[1] & ~3 | (words[2] & 0x3ffff) << 32)` divided by 16.
  Bits 32-33 select the convergence mode (2 = .DIV never branches, 3 = .CONV always).
- Two things the synthetic tests could not have told us:
  - 9 of the 53 branches carry a *second* condition in bits 87-90. The branch is taken only when
    both hold: the plain guard in bits 12-15 must be true for control to continue at all, and the
    extra predicate in 87-90 is what actually goes to the target. Modelling it as a conjunction is
    what the lowering does too (it emits `@extra BRA target` plus `@!guard BRA fallthrough`).
  - The opcode word with no predicate is not "zero in bits 12-15": `0x7947` already has 7 there, so a
    predicated test instruction has to *replace* those bits, not or them in. The first version of the
    tests silently produced more PT branches that way.
- Structuring: blocks split at branch targets, at BSSY reconvergence points and after terminators.
  A conditional branch whose target is the region end is a break; otherwise it is a diamond whose
  merge is the target of the branch at the end of the then-part. If-nodes take the fallthrough
  condition, so the guard is negated.
- State on the captured program: 91 control blocks, and structuring now runs until it meets an
  unimplemented opcode instead of failing on shape. The first blocker is `0x824`
  (`IMAD.U32 R5, R3, 0x60, R2`), which is arithmetic, not control flow.

## 2026-11-07 - Native frontend: integer and special-function families

- Added IMAD, LOP3, IADD3, P2R and MUFU, which takes the captured OoT compute program from 680 to
  452 unimplemented instructions (1884 at the start of the port, so 76% is covered).
- LOP3 is a bitwise truth table, not a predicate table. The Shannon expansion is valid bit by bit,
  so the generic case becomes `(a & f(1,b,c)) | (~a & f(0,b,c))` with constants 0 and ~0 at the
  leaves: 21 IR instructions for any of the 256 tables, and the constant folding pass collapses the
  simple ones. LUT 0xc0 answers `a & b`, which is what the captured `LOP3.LUT R0, R3, 0xfffc, RZ,
  0xc0` is doing.
- MUFU on SM70+ takes sine and cosine angles in *turns*, while the IR takes radians, so the native
  path multiplies by 2pi before the sine or cosine. The lowering does exactly the same
  (`0x40c90fdb`); getting this wrong would rotate a whole effect by a factor of 6.28.
- P2R is not a bitmask move in IR: each predicate is a separate per-lane value, so the register is
  rebuilt as `(src & ~mask) | sum(select(Pk, 1 << k, 0) & mask)`. One select per mask bit.
- A real trap: the coverage list is a `std::array<u32, N>` with explicit N. Declaring 52 entries
  while listing 51 leaves one value-initialised *zero*, which silently marks opcode `0x000` as
  implemented and breaks the padding rule. The count test caught it (`ImplementedCount() == 52`
  passed while the padding test failed). Count the initialisers, or let the compiler deduce N.
- Remaining top of the list is now `0x981` LDG (39), `0x589` (27), `0x986` (27), `0x810` (26),
  `0x388` (21), `0x984` (21), `0xabb` (20, ULDC) and `0xab9` (18) — memory access and uniform
  constant loads, which need address analysis rather than one-instruction translations.

## 2026-11-08 - Native memory access: LDG and ULDC

- LDG (0x981) assembles a 64-bit address from a uniform register pair (bits 32-37), an optional
  register offset (bits 24-31, a 64-bit pair when bit 90 is set, zero-extended otherwise) and a
  signed 24-bit immediate byte offset (bits 40-63). The IR side is a plain
  `LoadGlobalU8/S8/U16/S16/32/64` on that address, so the native path has none of the lowering's
  address-splitting constraints.
- The addressing mode is unambiguous in word 2: `0x081e8100` (with the base pair and the optional
  register) is the form seen here; the descriptor form (`0x0c1e1100`) is rejected for now rather
  than guessed.
- ULDC (0xab9 direct, 0xabb indexed) loads a constant buffer word or pair into the uniform register
  file: destination bits 16-23, width bits 73-75 (4 = 32-bit, 5 = 64-bit), byte offset bits 38-53,
  bank bits 54-58. The direct form maps straight onto `GetCbuf(bank, offset)`. The indexed form is
  deliberately not implemented yet: the units of its index register have not been confirmed against
  captured code, and guessing them would corrupt every load through it.
- Coverage on the captured OoT compute program: 452 to 375 unimplemented instructions, 54 opcodes.
  The structurer now emits 35 IR instructions before meeting an unsupported opcode, up from 24.
- Remaining tail: 0x589 SHFL with a register lane (27), 0x986 global store (27), 0x810 LOP3 with a
  predicate output (26), 0x388 and 0x984 shared memory (21 each).

## 2026-11-08 - Native frontend: stores, shared memory and the register shuffle

- STG (0x986) has the same address shape as LDG, with the data in bits 32-39, and IR takes the whole
  64-bit address for `WriteGlobal*`.
- LDS/STS (0x984/0x988) address shared memory as `Ra + (uniform base) + byte offset`, where a
  non-zero stride in bits 78-79 scales the register by `1 << (stride + 1)` bytes. IR takes the byte
  offset directly, so the pieces are added and the scale is one shift.
- SHFL with a register lane (0x589) is the immediate form with the lane read from bits 32-39: the
  mask (40-52) and mode (58-59) fields are identical.
- IADD3's immediate, constant buffer and uniform forms (0x810/0xa10/0xc10) only change where the
  second source comes from; the add is the same.
- Coverage on the captured OoT compute program: 375 to 268 unimplemented instructions, 61 opcodes
  (1884 at the start, so 86%). The remaining tail is 0x388 shared store with a register offset (21),
  0x306 (16), 0x209 FMNMX (15), 0x892 (14), 0x3c2 (13), 0xd06 (12) — a long tail of 56 distinct
  opcodes, each with a handful of uses.

## 2026-11-08 - Native frontend wired into the pipeline behind EDEN_NX2_NATIVE

- The stage setup and optimization pipeline moved out of `Maxwell::TranslateProgram` into
  `Maxwell::FinalizeProgram`, so both frontends feed the same passes. Without that, the native path
  would have been a second, silently diverging pipeline.
- `Shader::Native::TranslateProgram` builds the syntax list, blocks and post-order from the
  structurer and then calls `FinalizeProgram`. It throws when the structurer cannot translate an
  instruction, which is what lets the shader cache fall back per program.
- The captured native instructions now live in `LoweredNativeCode::native`, exposed through
  `Environment::NativeInstructions()`, because the environment's `code` is the *lowered* Maxwell
  stream. The native frontend needs the original words.
- `EDEN_NX2_NATIVE=1` makes the Vulkan pipeline cache try the native frontend first and fall back to
  the lowering on any exception, logging which happened per shader hash. The OpenGL path is not
  wired: the experiment targets Vulkan.
- Tests: two new cases run the whole pipeline on a two-instruction program (and check that an
  untranslatable program throws so the fallback happens), which is the first end-to-end exercise of
  the native path rather than a syntax-list assertion.
- Nothing renders through the native path yet: every captured program still stops on an
  unimplemented instruction, so the fallback is the path actually taken until the tail is clear.

## 2026-11-08 - Native frontend: clearing the tail

- Added STS with a register offset (0x388), FMNMX (0x209), I2F.U8/S8 (0x306, 0xd06) and SHF with an
  immediate (0x819): 268 to 198 unimplemented instructions, 66 opcodes, 86% to 90% of the captured
  compute program. The structurer now gets 59 IR instructions in before it stops, up from 35.
- FMNMX's direction is the predicate in bits 87-90, and a negated predicate *swaps* min and max
  rather than selecting a different instruction: `@!PT` is max, `@PT` is min. The lowering encodes
  the same thing by swapping the two Maxwell values.
- I2F takes a byte selected by bits 60-61 and converts it; signedness is bit 74, independent of the
  selector. `ConvertIToF(32, 8, signed, byte)` is the IR call, not `ConvertU32F32`.
- Two mistakes worth recording, both invisible without running:
  - `Translator::Error()` returned a `string_view`. Once the message needed the opcode number it
    pointed into the translator's own buffer, so the structurer's copy dangled and the sweep
    reported a garbled string. The structurer now owns a `std::string`.
  - Naming the opcode in the failure was worth more than any single opcode: the coverage report went
    from "unsupported native opcode" on every program to the exact number blocking each one.
- Next blockers, in program order: 0x811 LEA immediate (the current stop), then 0x807/0x808
  LOP3 variants, 0x892 ULOP3 and 0x899 USHF.

## 2026-11-08 - Native frontend: LEA, and the first deliberately unguessed gap

- LEA (0x211/0x811/0xa11/0xc11) is `Rb + (Ra << s)` with `s` in bits 75-79; LEA.HI takes the high
  word of the 64-bit shift instead, and `.X` adds the carry predicate from bits 87-90. The plain form
  can also write a carry-out predicate into bits 81-83, computed as "the 64-bit sum has a nonzero
  high word".
- Coverage: 198 to 184 unimplemented instructions, 70 opcodes, and the structurer now emits 60 IR
  instructions. On the captured program it stops on `0x211` with a *second* carry output in bits
  84-86.
- That second output is deliberately not implemented. The lowering treats bits 81-83 and 84-86 as a
  pair (`first`/`second`) and compares a 64-bit high word against 1 and 2 to fill them, but what the
  second predicate means has not been confirmed against a decoded instruction. Guessing it would
  silently produce a wrong 64-bit address for every consumer of that predicate, so the frontend
  reports `native dual carry output` and lets the program fall back to the lowering.
- Learning: the coverage histogram and the "where does it stop" answer have diverged. The histogram
  now has 52 opcodes at a handful of uses each, while the stop is a single instruction whose
  *semantics*, not encoding, is missing. That is the difference between "add another opcode" and
  "go and find out what the hardware means".

## 2026-11-08 - Native frontend: R2UR, USHF and the LEA carry question settled

- The "dual carry output" that blocked the program turned out not to be a gap at all: bits 84-86 of
  LEA are a second output that the *lowering ignores outright*, and the lowering's programs render
  correctly with it dropped. Matching that shipped behaviour is evidence-based, not a guess, and it
  took the program from 60 to 90 IR instructions in one step. The lesson is to check what the
  shipping path does with a field before treating it as unknown.
- R2UR (0x3c2) broadcasts the first lane satisfying the execution predicate into a uniform register.
  IR has `SubgroupBallot` but no "find lowest set bit", and `ballot & -ballot` isolates exactly one
  bit, whose index `FindUMsb` then returns. That is exact for a 32-lane subgroup and needs no
  helper opcode.
- USHF (0x899) is the uniform-file version of the immediate SHF: the word-2 patterns identify which
  word is shifted (`0x08011600 | high` for the high word, `0x0800163f` for a right shift of one
  register, `0x0800063f` for a left shift).
- Coverage: 184 to 161 unimplemented instructions, 72 opcodes, and the structurer now emits 100 IR
  instructions before stopping (60 at the start of this batch). The next stop is 0x28c, a uniform
  compare that writes a uniform predicate.

## 2026-11-08 - Native frontend: uniform predicate producers and the wide multiply

- UISETP (0x28c/0x88c) is the first instruction that *writes* a uniform predicate: compare two
  uniform registers with the same `IntegerCompare`/`PredicateCombine` the regular path uses, then
  store the mask through `SetUniformPredicate`. Its guard is a uniform predicate, not a per-lane one.
- UIADD3 with an immediate (0x890) and UIMAD.WIDE.U32 (0x8a5) round out the uniform arithmetic.
  The wide multiply writes a uniform register pair, so it needs a 64-bit multiply — and IR has none:
  `IMul` is 32-bit only and there is no `IMul64` opcode. Since the multiplier is a constant, the
  product is built as the sum of the multiplicand shifted for each set bit, which is one term for
  the common `* 0x10` shape.
- Diagnostic improvement that paid for itself immediately: a type error thrown while building IR used
  to surface as "compilation_failed: Incompatible types U64 and U32" with no clue which instruction.
  `TranslateRange` now catches it and reports `instruction 35 (0x8a5): ...`, which found the bug in
  one run.
- That bug was mine and instructive: `ir.IMul` accepts `const U32&` only, and the U64 arguments were
  converted down rather than rejected, so the failure appeared later as a type mismatch on the result.
- Coverage: 161 to 146 unimplemented instructions, 76 opcodes, and the structurer now emits 254 IR
  instructions. The program reaches its *first real form gap*: a global load whose word 2 is the
  descriptor form (`0x0c1e1100`) rather than the plain uniform-base form.

## 2026-11-08 - Native frontend: the program reaches its 64-bit address chain

- Coverage: 146 to 392 IR instructions emitted before the stop. The tail opcodes were the easy part;
  what moved the number was getting the *forms* right.
- LDG has a second addressing form the lowering rejects: `LDG.E.STRONG.SM Rd, [???255.64+UR12]`,
  word 2 `0x0c1ea900`. The address is the same uniform pair; bits 75-77 carry the memory ordering
  (`.STRONG.SM`, `.CONSTANT`) and are not part of it. Strongly ordered loads become coherent loads.
  This is one of the programs the *lowering itself* cannot translate, so the native path is ahead of
  it here rather than matching it.
- IADD3 with carry outputs (word 2 `0x0f91e000`): the lowering derives the two predicates from the
  64-bit sum as "high word >= 1" and "high word >= 2", which encodes a carry count of 0, 1 or 2. The
  native path computes the same sum and derives the same two conditions.
- The stop is now `IADD3.X R5, RZ, UR7, RZ, P0, P1` (instruction 47): the chained form that consumes
  the pair as its carry-in. The carry count reading implies carry_in = P0 + P1, but that has not been
  confirmed against captured consumers yet, so it is left refused.
- Diagnostics: the translator's failures now carry the instruction index and opcode, and a type
  error thrown during IR construction is caught with the same context. Both were needed to find
  bugs in this batch at all.

## 2026-11-09 - Native frontend: from 392 to 1111 IR instructions

- The batch: IADD3.X (the chained add), LDG with a plain 64-bit register-pair address (0x381), USEL
  (0x287/0x887), UIMAD (0x8a4) and the indexed ULDC (0xabb). Together they take the captured compute
  program from 392 to 1111 emitted IR instructions — 58% of its 1920 source instructions — before the
  next stop, `0x292`.
- IADD3.X is settled by the lowering's own code, not by inference: it adds *two* carry predicates,
  read from bits 77-79 and 87-89 with negation at 80 and 90. That matches the pair the producer
  writes (high >= 1, high >= 2) and the captured `IADD3.X R5, RZ, UR7, RZ, P0, P1`, where bits 77-79
  hold P1 and 87-89 hold P0.
- The indexed ULDC's index register is a byte offset. Evidence: the lowering maps it to Maxwell LDC,
  and `load_constant.cpp` computes `ir.IAdd(reg, imm_offset)` with no scaling. That settles the
  question left open two batches ago, when guessing the units would have corrupted every load.
- Learning: the biggest jumps came from *forms* of instructions already present (LDG address modes,
  IADD3 carry chains, indexed constant loads), not from new opcodes. The histogram's "unimplemented"
  count understates this: several of the forms were inside opcodes already marked implemented.
- Diagnostics continue to pay: every bug in this batch (a missing dispatch case, a `Value` that
  needed an explicit `IR::U32` wrapper) was found from one report line naming the instruction.

## 2026-11-09 - Native frontend: 1397 IR instructions, and one crash worth stopping for

- ULOP3 (0x292/0x892) and I2FP 32-bit (0x245/0xa45/0xc45) take the captured program from 1111 to 1397
  emitted IR instructions. Uniform index 63 is the zero register (URZ), not a real uniform register,
  so reads of it become a constant zero.
- Then STL/LDL (0x387) turned up, and translating it exposes a hard crash (exit 0xC0000005, no JSON,
  no exception) past instruction 164 - `CS2R R2, SRZ`. A per-instruction trace behind
  `EDEN_NX2_TRACE_INST` showed the translation of 161 (STL) succeeding and the process dying at 164.
- Both are **refused again** rather than shipped, because a crash cannot fall back: the shader cache
  falls back on an exception, and an access violation walks past the handler and takes the emulator
  down. The rule that comes out of this: a new translation is only safe to enable when the process
  survives its failure, and a refusal is always better than an uncaught fault.
- The crash itself is not yet explained. What is known: it needs both the STL translation and CS2R to
  be reached, it is an access violation rather than a stack overflow, and the translator's own prints
  show the IR for 161 completing. The next step is a debugger or a checked build, not more printing.

## 2026-11-09 - The "translator crash" was a report bug, and 44 more instructions

- The access violation past `STL` was **not** in the translator. `Structurer` reserves the `If` node
  with `syntax_list.emplace_back()` before filling it in; `AbstractSyntaxNode`'s `type` defaults to
  `Block` and `data` zero-initialises to a null block pointer. When a later instruction failed, the
  unfilled node stayed in the list, and `shader-sweep` counted IR with `node.data.block->size()` -
  a null dereference. Anything that read a partially built syntax list would have done the same.
  Fixed at the source (the reserved node is typed `If` immediately) and in the report (a null block
  is never dereferenced). The translator, `STL`/`LDL` and `CS2R` are all fine and are now enabled.
- Locating it took a linker map, not symbols: `/MAP` added to the existing link line, a
  `SetUnhandledExceptionFilter` that prints `RtlCaptureStackBackTrace` addresses, and the map's
  `Rva+Base` column to name the frame. `main + 0x3962` was enough to find it, because the faulting
  code was inlined into `main`.
- New diagnostic: `shader-sweep --native-all` records every blocked instruction and keeps going
  (`Structurer::SetTolerant`). A program that stops at the first refusal hides the shape of the work;
  one tolerant run listed all 69 blocked instructions of the captured program at once. The IR from a
  tolerant build is not valid output and the flag only exists in the sweep.
- Field layouts confirmed with nvdisasm before use: `IMNMX` (dest 16-23, bit 73 signed, predicate
  87-90 - bits 72-73 are the instruction's own flags, not source modifiers, which is also why `ISETP`
  was failing "integer source modifier": its `EX` bit lands where the generic decode expects `abs`),
  `POPC` (tilde is bit 63), `SEL`/`FSEL` (predicate 87-89, negation 90), `PRMT` (selector in word 1,
  second source 64-71), `SHF.L.U32.HI` (shift in word 1, `.W` in bit 75 wraps the shift to five bits,
  otherwise it saturates at 32, so the `0xffffffff` idiom is a register pass).
- Uniform-source forms mark bit 91, which several emitters were rejecting as an unknown mode bit:
  `FFMA` (0xe23), `LEA` (0xc11) and `IADD3` (0xc10). That one bit explained three separate refusals.
- Coverage on the captured OoT compute program: 14552 emitted IR instructions, 106 opcodes, and 69
  blocked instruction instances down to 37. Remaining work is concentrated: `VOTE`/`VOTEU`,
  `ATOMG`/`REDG`/`REDUX` (warp- and memory-atomics), `F2I`/`F2IP`, `ULEA`, `UIADD3.X`, `UIMAD`,
  `LDC.U8`.

## 2026-11-09 - F2I and UIMAD, and what is left

- `F2I` (0x305) is the word form only: truncate, clamp to `[0, 2^32)`, convert unsigned, and turn
  NaN into zero, which is exactly the clamp the Maxwell F2I path performs. Signed destinations and
  the byte forms (`F2IP`) stay refused; they need their own evidence.
- `UIMAD` with a uniform multiplier (0x2a4/0x2a5) reuses the lowering's mask for 0x2a4
  (`(words[2] & ~0x080002ff) == 0x078e0000`) because the plain one rejects a legitimate form. The
  wide form expands the product into four 16-bit partial products: IR has no 64-bit multiply and a
  32-bit one truncates the cross terms.
- Coverage: 15075 IR instructions, 109 opcodes, **28 blocked instruction instances** left, all in
  three families: warp voting (`VOTE`, `VOTEU`), warp and memory atomics (`REDUX`, `ATOMG`, `REDG`),
  and the remaining uniform/address forms (`ULEA`, `UIADD3`/`UIADD3.X`, `LDC.U8`, `F2IP`).
- Those three families share a requirement the rest of the port did not have: they need the warp's
  active-lane set (a ballot) or an atomic IR op, so they are the next real design step rather than
  more field decoding.

## 2026-11-09 - Warp voting, atomics, uniform arithmetic: only F2IP is left

- `VOTE`/`VOTEU` (0x806/0x886) are `SubgroupBallot` plus `VoteAll`/`VoteAny`/`VoteEqual` chosen by
  bits 72-73; the captured VOTE discards its predicate result (output field is PT), so only a ballot
  reaches the block, while VOTEU writes a uniform predicate. The "register" a VOTEU ballot would go
  to is 63, which is URZ, i.e. discard.
- `REDG`/`ATOMG` (0x98e, 0x3a8, 0x9a8) map straight onto the Maxwell `RED`/`ATOM` enumerations: the
  type in bits 73-75 selects unsigned/signed 32-bit or float (0/1/3), and bits 87-89 are the
  operation the `AtomOp` enum already uses (ADD 0, MIN 1, MAX 2, INC 3, DEC 4, AND 5, OR 6, XOR 7).
  A REDG has no result, so the value-returning IR atomic is used and discarded - the memory effect
  is the same. Addresses are register pairs or uniform pairs, with a signed 20-bit byte offset.
- `UIADD3`/`ULEA` (0x890/0x290, 0x891/0x291) reuse the register forms' semantics over the uniform
  file: the plain sum and its two carry outputs from the 64-bit intermediate, the `.X` chain adding
  two carry-in predicates, and a uniform guard that keeps the old target value. For ULEA only the
  flags the shipping lowering itself checks (`!74`, `!80`, carry field) are validated - whatever
  else those words carry, programs render correctly with it ignored.
- `LDC.U8` (0xb82) is the Maxwell LDC's own address shape: default mode is `bank + (register +
  offset)`, mode 2 folds the register into a flat constant-buffer address whose high half is the
  bank.
- `REDUX` (0x3c4) becomes five `ShuffleButterfly` steps with the inactive lanes - taken from
  `SubgroupBallot(true)` against this lane's `SubgroupEqMask` - replaced by the operation's identity
  (zero, or all ones for a minimum), exactly as the shipping lowering does. Every lane ends with the
  same value, which is what makes writing it to a uniform register valid.
- Remaining: `F2IP` (0x243, two instances). Its mnemonic says `F2IP.U8.F32.NTZ` with four operands,
  and the form in this program is **not** the one the lowering accepts (`words[2]` is 0x000004ff, the
  lowering requires 0x0000c4ff, and its source register field is 40 rather than 255), so there is no
  shipping behaviour to copy and no evidence for what the extra operands do. It stays refused rather
  than guessed.
- Coverage: 119 opcodes, 15374 IR instructions in the tolerant pass, and the strict pass now stops at
  instruction 1575 of 1920.

## 2026-11-09 - Corpus measurement: 3 of 4008 compute programs, now 3115

Measuring all 4008 compute programs captured from Ocarina of Time with
`shader-sweep --native` changed the priorities completely. Before this round **3 programs**
translated end to end; the failures were concentrated in four gaps, not spread evenly:

| blocker | programs |
|---|---|
| 128-bit `LDG` (size 6) | 2818 |
| `F2I` forms beyond the unsigned word | 2405 |
| `IMAD` with a register or constant-buffer multiplier (0x224/0x424) | 1484 |
| `IMAD.WIDE` (0x825) | 599 |

- 128-bit loads were simply missing (size 6); with pairs and quads added, the count reached 1859.
- `F2I` was refused for signed destinations and for any rounding but truncation. The word form is
  one instruction: round per bits 78-79 (Maxwell's own enumeration: round-even, floor, ceil,
  truncate), clamp to the destination range, convert, fold NaN to zero. That alone took the count
  from 1859 to 2622.
- The `IMAD` forms **move their operands around**: 0x824 takes the multiplier as an immediate with
  the addend in bits 64-71, 0x224 the reverse for the multiplier (register) and 0x424 swaps them
  again (multiplier in bits 64-71, immediate addend in word 1). 0xa24/0x624 read the multiplier or
  the addend from a constant buffer. nvdisasm caught this: a first attempt decoded 0x424 as a
  constant-buffer multiplier and produced `IMAD R28, R0, R1, 0x100` semantics that were wrong in a
  way the translations still accepted. Re-check every operand order against the disassembly, not
  against a neighbouring form.
- `IMAD.WIDE`/`IMAD.HI` reuse the four-partial-product expansion from `UIMAD.WIDE`; the signed form
  corrects the high word by subtracting `b` when `a` is negative and adding `a` when `b` is, which
  is what the shipping lowering does.
- `SGXT` (0x81a) is a shift: logical for `.U32`, arithmetic when bit 73 is set, by the immediate in
  word 1.
- `IADD3` has a third accepted word shape (0x0791e000, carry outputs without the uniform bit) and a
  multiplier of one in `IMAD` is the `IMAD.IADD` alias, which the generic product already computes.
- Result: **3115 of 4008** compute programs translate end to end (78%), up from 3. What is left is
  dominated by textures (`TLD` 0xb60/0xb66, `TXQ` 0xb6f/0xb63, 0xb97) and a handful of forms:
  indexed `LDC` modes, the remaining `IMAD` carry modes and `0x307`.

## 2026-11-09 - First real compile: the structurer has no loops

`shader-sweep <program> --native --output out.spv` now builds the whole program the way the pipeline
cache does and runs it through the SPIR-V backend, so the structurer is exercised by a real compile
instead of only counted. First result: **0 of 47 sampled translating programs pass `spirv-val`**.

Two structural bugs were found and fixed on the way:

- **Reused merge blocks.** `if (a) goto M; if (b) goto M;` makes M the merge of two headers, which
  SPIR-V forbids. A block that is already a merge now gets a fresh single-branch block in front of
  it (`UniqueMerge`).
- **Third successors on terminated blocks.** The fallthrough branch was appended even when the
  previous block already had its terminator, giving blocks three successors. Terminated blocks are
  now tracked and never extended.

With those fixed, every remaining failure is the same: `OpPhi's number of incoming blocks (2) does
not match block's predecessor count (3)`. The cause is not the phis. Every translating program in
the corpus contains exactly one **backward** `BRA` (a loop) and one `BSSY`, and the structurer only
builds if/else diamonds: there is no `Loop`/`Repeat` node anywhere in `control_flow.cpp`. A back
edge therefore produces a "diamond" whose body is empty and whose body entry is a node that was
never created, which is where the phantom predecessor comes from.

- The next step is loop structuring, and it should be built from the hardware's own regions rather
  than inferred merges: `BSSY <target>` names a reconvergence point, `BSYNC` closes the region, so a
  back edge to a region's entry is a loop whose merge is that reconvergence point. That gives the
  `Loop`/`Repeat` shape the SPIR-V backend needs, with a real condition on the repeat edge.
- `SHADER_SWEEP_IR=1` prints the block graph (order, successors, predecessors), which is how the
  phantom predecessor was traced; it stays as a diagnostic, off by default.

## 2026-11-09 - The structurer was fighting the backend's block ordering

**3115 of 3115 translating programs now pass `spirv-val`** (was 0). The fix was not in the code the
translator emits; the structurer was building a block graph that disagreed with the graph the SPIR-V
backend actually emits.

The backend's `Traverse` does not read `Block::AddBranch` at all. A `Block` syntax node emits
`OpBranch` to **the next `Block` node in the syntax list**, and only `If`, `Loop`, `Repeat`, `Break`
and `Return` nodes emit their own terminators. So every block's recorded successor must be the node
that follows it. The old code broke that invariant three ways:

- it placed trampoline blocks (`UniqueMerge`) at creation time, *before* the `If` node, so the `If`
  header became the trampoline instead of the block holding the condition;
- it appended the merge block wherever the enclosing loop reached it, not immediately after `EndIf`,
  so the fallthrough branch went somewhere else;
- an unconditional `EXIT` inside a body recorded a branch to the return block but emitted nothing,
  and the following `EndIf` branched to the wrong target.

The rule now: an `If`'s merge block node is emitted immediately after its `EndIf`, a `Return` gets
its own epilogue block emitted in place, and a divergent tail is not emitted at all (otherwise the
SSA pass removes the unreachable blocks from `program.blocks` while the syntax list keeps their
nodes, and the last `OpLabel` gets result id 0).

Two more findings:

- A conditional `EXIT` cannot be a `Break` to the return block: branching out of a selection to a
  block beyond its merge is "Selection must be structured". It becomes a selection whose body is a
  return block (`If` + `Block(return)` + `Return` + `EndIf`).
- Loops are rare in this corpus: only 196 of 4008 programs have a backward branch, and none of those
  196 translate yet (they use textures). Loop structuring is now built from the back edge itself: the
  target block is the header, a fresh empty block carries `Loop` so the header's own terminator does
  not collide with it, the latch block is the continue block, and its back edge becomes `Repeat`.
  A header with two back edges (two `continue`s) is refused rather than merged.

Measurement: 3115 validated, 0 invalid, 893 still blocked on translation (textures dominate).

## 2026-11-09 - Textures: operand packing, not the sample itself

`TEX` (0xb60) was the largest remaining blocker (385 programs). The sample maps straight onto the
IR `ImageSample*` family; the work is unpacking NVN2's operand list, which is the same packing the
shipping lowering unpacks into Maxwell's coordinate and meta registers:

- The list is **array index first, then coordinates, then LOD/bias, offsets and the depth
  reference**. Values live in `Ra`/`Rb` as halves of the count (`in_a = (count + 1) / 2` for up to
  four values, otherwise 4 in `Ra`). A source register of 255 is zero.
- The dimension field is **not** Maxwell's enumeration: 0 1D, 1 2D, 2 3D, 3 CUBE, 4 ARRAY_1D,
  5 ARRAY_2D, 6 unused, 7 ARRAY_CUBE. nvdisasm confirms it (field 1 prints `2D`, field 2 prints
  `3D`).
- An array coordinate is a **U16 converted to float** and appears last in the IR coordinate vector
  while it is first in the native operand list.
- LOD modes: 1 `LZ` is an explicit level 0, 2 `LB`/6 `LBA` are implicit with a bias, 3 `LL`/7 `LLA`
  are explicit levels, 4/5 are invalid. The descriptor is a constant-buffer byte offset
  (`bits(40, 13) * 4`), which is exactly the immediate handle the texture pass expects.
- Split destinations (`second_dest` != 255) are ordinary: components 2 and 3 go to `second_dest`.
  Only the four-component form needs the pair aligned, because it packs RG and BA.

Coverage: 3115 -> 3441 translating, and all 3441 still pass `spirv-val`. 567 remain, now led by
`TXQ` (0xb6f), `TLD` (0xb66), `TLD4` (0xb63) and 0xb97.

`LDC.64` (the 64-bit indexed constant load) is added too: it fills a register pair from one
`GetCbufU32x2`.

## 2026-11-09 - The rest of the texture family and the numeric conversions

`TLD` (0xb66) and `TXQ` (0xb6f) follow the same descriptor rule as `TEX`: the handle is the
constant-buffer byte offset in bits 40-52 times four. `TLD` reads whole texel coordinates, one
register each from `Ra` (the array layer is the low half of `Ra`, with `x` in `Ra+1` and `y` in
`Rb` when a level follows); `TXQ` takes the mip level from bits 24-31 and asks for the mip count when
mask bit 3 is set.

Small gaps closed while measuring (each confirmed against a captured word):

| opcode | meaning |
|---|---|
| `0x91a` | `DEPBAR`, a scoreboard wait: no IR, data dependencies already order the loads |
| `0xd00` / `0xd09` | `FLO` / `POPC` with a uniform source: bits 9-11 are 6 instead of 1 |
| `0x307` | `FRND`: round to an integral float, bits 78-79 select the rounding |
| `0x306` halfword form | `I2F.U16/S16`: bits 60-61 are the half index, and the mode word is 0x101000 |
| `0xa09` | `FMNMX` with a constant-buffer source |
| `0x2be` | `UBREV`: `BREV` on a uniform register (bits 9-11 stay 1) |
| `0x305` small/byte | `F2I` to a 16- or 8-bit destination: the mode word selects the width and the
  result still fills the whole register, clamped to that width |

One real bug: the store address check rejected `STG` data registers above `0x3f`, because bits 38-39
belong to the data field, not to the address. The mask check on the mode word is the whole
condition; the extra bit test was copied from the load path where it means something else.

Coverage: 3441 -> **3564 of 4008** translate (89%), all 3564 pass `spirv-val`. What is left is
surfaces (`SULD`/`SUST`, 79), the `IADD3`/`IMAD` carry forms (80), calls (`0x944`, 36), `TLD4`
(35), shared-memory widths (22) and `F2FP`/`R2P`.

## 2026-11-09 - The native frontend only ran on a cold cache

Live A/B on Ocarina of Time (Kokiri Forest save, `EDEN_NX2_NATIVE=1` against the lowered path):

- `IPA` (0x326) and `ALD` (0x321) block every graphics shader at instruction 0, so the native
  frontend cannot build a graphics program yet.
- The compute programs it does handle behaved as expected, but the first run showed **zero** native
  attempts and that exposed a design hole: a warm pipeline cache hands the pipeline builder a
  `FileEnvironment`, which had no `NativeInstructions()`. Only `GenericEnvironment` (a fresh program
  lowered this run) provided the captured stream, so `EDEN_NX2_NATIVE` did nothing at all once the
  cache was warm. The flag was effectively "first run only".

Fix: the captured native instruction stream is now serialized with the environment in the pipeline
cache, and `FileEnvironment` returns it. `VulkanPipelineCacheVersion` went 50 -> 51 so old entries are
rebuilt.

Verified live: cold run without the flag serializes 707 graphics + 80 compute pipelines; the next
run with the flag on that warm cache reports 1294 native attempts (127 translated, 1167 fell back),
where before the fix it reported none.

Measured, not assumed: a cold-cache native run reached the same save with the image matching the
baseline within temporal noise (mean 5.9 per channel across the run boundary against 5.4 between two
frames of the same run), with 112 of 1229 programs translating natively.

## 2026-11-09 - ALD, IPA and ISBERD, and the four bugs behind the first native graphics frame

The graphics gate was `ALD` (0x321), `IPA` (0x326) and, for Switch 2 task/mesh programs, `ISBERD` /
`ISBEWR` (0x923/0x927). All three are now translated straight to IR with the field layouts from the
lowering, checked against nvdisasm on captured words:

- `ALD`: dest 16-23, vertex 32-39, byte address 40-49, components 74-75 (+1), patch 76, output 79.
  Indexed and patch/output reads are refused, matching the lowering's own guards.
- `IPA`: attribute 64-71, sample mode 76-77, constant bit 78; non-indexed pass/centroid only. The
  perspective rule is the Maxwell one: legacy attributes stay perspective, generics follow the
  pixel shader input map, and an unused entry also means perspective.
- `ISBEWR`/`ISBERD`: address register 24-31, byte offset 40-63, `.32` 75, region 76-77 (0 = map,
  3 = attributes), write 79. Task/mesh only.

Corpus check on Ocarina of Time's 37,947 captured programs: 67,975 `ALD`, 407,967 `IPA` and 21,956
`ISBE` instructions, every one inside the accepted subset. The opcode list went 137 -> 141.

Live A/B on the Kokiri Forest save, warm pipeline cache: 1,200 native attempts, 42 distinct
programs translate (0 graphics programs before), image matching the lowering within the run's own
temporal noise (mean 5.06 per channel against ~5.4 between two frames of one run). Remaining
fallbacks: `AST` 0x322 (402), `FRAG` 0x95b (171), extended integer compares (84), uniform shifts
(68), then `0xc17`, `0x20a`, `0xb63`.

Four bugs had to be found and fixed before that frame rendered:

- **Driver crash on a mesh program.** `PatchMeshInstructions` rewrites a marker `OpStore` into
  `OpSetMeshOutputsEXT` but matched only the single marker recorded in `EmitContext`. A program with
  more than one exit has one epilogue per exit and therefore several markers, each with its own
  primitive-count id; only the last was patched and the rest stayed `OpStore <constant>`, which is
  invalid SPIR-V. spirv-val reports `OpStore Pointer <id> is not a logical pointer` and the NVIDIA
  driver crashes. Fix: match the marker by its constant vertex-count pointer alone.
- **Native fragment programs rendered nothing (black screen).** The fragment color of an NVN2
  program is in R0 and up at the exit, and the Maxwell frontend's EXIT maps those registers to
  render targets and components from the pixel shader header. The native structurer only created an
  epilogue block and never emitted those stores. Fix: `EmitFragmentExit` on every exit path.
- **`FMNMX`/`IMNMX` inverted the swap bit.** The negation bit (90) is a min/max swap, not a negated
  predicate: the Maxwell rule is `if (neg) swap(min, max); Select(pred, min, max)` with PT reading
  as true. The native frontend used it as `GetPred(pred, negated)` too, so a cube-map coordinate
  normalisation that must be a maximum became a minimum and the sky and terrain went black. The
  existing unit test only covered a non-negated instruction, which is why it passed.
- **Loop latch blocks had no successors.** The `Repeat` node branches to the loop header and merge,
  but the IR block had no matching successors, so the merge had no predecessor; a pass dropped it
  while the syntax list kept it and the backend emitted an unset label. Fix: `BranchTo` header and
  merge from the latch.

Tooling: ported upstream `EDEN_NX2_PIPELINE_BUILD_TRACE` (c87f09a84c) and extended it to record the
native frontend's per-program result; added `EDEN_NX2_SPIRV_DUMP` (every graphics stage's SPIR-V
plus the captured native stream) and `EDEN_NX2_NATIVE_STAGES` to restrict the native frontend to
named stages while bringing one up. The crash, the black screen and the dark image were all found
with those three.

Open: mesh/vertex native still shades some meshes too dark (the character in the Kokiri Forest save),
so `EDEN_NX2_NATIVE` defaults to compute and fragment; `EDEN_NX2_NATIVE_STAGES=mesh,vertex` opts in.

## 2026-11-09 - The merge to upstream's shader cache v2, and where graphics coverage actually stops

The fork and upstream had both grown a pipeline precompilation scheme. Upstream's won: shader cache
v2 records each pipeline as the guest environments its shaders were translated from, in an
append-only log, with a driver layer on VK_KHR_pipeline_binary; `shader_warm` and `PrecacheShaders`
build a title's pipelines before play starts. This branch's per-title key list, its prefetch path
and `keylist_tool.py` were removed with the merge (213 upstream commits, 26 conflicting files).

What moved and why:

- **The native stream was already in the cache.** v2 stores a native program's captured
  instructions (`ProgramRecord::native`) and rebuilds the lowered code from them, so the fork's
  inline copy of the stream in the environment record was redundant. `MakeLoweredNativeCode` now
  fills `LoweredNativeCode::native` from that span and the environment layout is upstream's again:
  cache version stays 54, 53 and 52 stay compatible, and migration from an older cache works.
  Live check on a warm cache: 87 graphics programs translate and 313 fall back, so the stream does
  reach the frontend through the record.
- **The debug server kept this branch's tools** (pause, resume, step, shader inventory, GPU memory
  map, read and write GPU memory, input recording and playback) on upstream's server, which now
  passes a client-gone signal to handlers.
- **The lowering's indexed ULDC rules are upstream's**: an index register can align the offset, so
  the indexed form accepts offsets the fork refused. The two tests that encoded the stricter rule
  follow it.
- Two MSVC STL problems surfaced and are fixed at the call sites: `std::ranges::find` instantiates
  `_Find_vectorized`, which only accepts 1, 2, 4 and 8 byte elements and now sees a 12-byte
  `StorageBufferAddr` and a 32-byte `PipelineRecord`.

Graphic coverage, measured after the merge on the Kokiri Forest save:

| Stage set | Translated | Fell back |
|---|---|---|
| `fragment` (with compute) | 87 | 313 |
| `vertex,mesh` | 0 | 2 |

Every vertex program in this title stops at `AST` (0x322) on its first instruction, so no mesh or
vertex program reaches the frontend at all. The darker character seen earlier with
`EDEN_NX2_NATIVE_STAGES=vertex,mesh` is pose and shadow: that run had no native programs either, and
a run with the same stage set renders the character exactly as the baseline. So the "native mesh
writes wrong attributes" hypothesis is untested rather than disproved: there is no translated mesh
program to compare yet. `AST` (0x322), then `FRAG` (0x95b) and the extended integer compares
(~84 fallbacks) are the gate.

## 2026-11-10 - AST translates, and what that exposes in the vertex path

`AST` (0x322) is ALD's counterpart: it writes one to four float attributes for the vertex named in
bits 64-71, at the byte address in bits 40-49. A one-component store to `a[0x5c]` is the
per-primitive hint that nothing reads back, so it is dropped exactly as the lowering drops it.
Ocarina of Time's vertex programs write the whole position with `AST R0, a[0x70]`, so nothing in
that stage could translate before this.

All 18 `AST` instructions in the captured vertex corpus fall inside the accepted subset (no patch,
output or indexed form), and the opcode list went 141 -> 142.

Live, with `EDEN_NX2_NATIVE_STAGES=vertex,mesh` and a cache cleared for the test: **301 programs
translate and 116 fall back**, against 0 translated before. `AST` no longer appears as a fallback;
the largest remaining reason is IADD3 (0x810) "native add carry mode is not implemented".

**The 3D scene then disappears when vertex is native** (a white frame with the 2D HUD intact),
while the lowering renders the same save correctly. Measured so far:

- The IR is right: a captured vertex program keeps its six `SetAttribute` instructions through the
  whole pipeline (`[native]` test in `native_program.cpp`), and the SPIR-V it emits from that IR
  stores every position component (`OpStore` to the four `gl_Position` access chains) and passes
  `spirv-val`.
- Skipping native for dual-vertex pipelines (the guest's split vertex programs, joined by
  `MergeDualVertexPrograms`) did not change the frame, so the merge is not the cause.
- Fragment and compute native are unaffected; only the vertex stage breaks the image.

So the fault is above the per-program translation: it is in how the produced vertex programs enter
the pipeline (pipeline state, attribute bindings, or which draws they serve). The next step is a
frame dump (`EDEN_NX2_FRAME_DUMP`) with vertex native, to find the first pass whose input is wrong,
rather than more per-program comparison.

## 2026-11-10 - The vertex white frame: what it is not

Followed the frame dump through, with the save restored from `D:\git_repos\nswitch2\lichii-test-save`
before each run so both runs start from the same place.

- **The frame dump answers it at pass 082**: the baseline's pass 082 (draws 790-831) holds the
  scene; the native run's matching pass holds the clear colour and only the HUD. Both runs execute
  the same number of draws in that pass, so nothing is being skipped: the geometry is submitted and
  lands outside the view, which fits wrong positions rather than a missing draw.
- **Not dual-vertex.** No pipeline in this title builds with an `s0:` stage hash, so VertexA is
  never present and `MergeDualVertexPrograms` never runs. The earlier experiment that skipped
  native for dual-vertex pipelines therefore could not have changed anything, and the guard was
  reverted rather than kept on a guess.
- **Not the pipelines being dropped or rejected.** The log shows no `Skipping ...` and no driver or
  validation failure; 298 vertex programs translate and 117 fall back on a cleared cache.
- **Not the translation of a program in isolation.** A captured vertex program keeps its six
  `SetAttribute` instructions through the whole pipeline and the SPIR-V stores all four
  `gl_Position` components.

So the remaining candidate is the *content* of what the native vertex programs compute: the ALD
inputs, or the arithmetic feeding the stores, differ from the lowering's for the same words. The
next step is a SPIR-V comparison of one program between the two frontends on the current build
(both dumps of `EDEN_NX2_SPIRV_DUMP` for the same hash), starting from its `in_attr` reads, rather
than more whole-frame work.

## 2026-11-10 - The white frame was a missing prologue

Comparing the same vertex program's SPIR-V from both frontends found it in the smallest one: the
lowered program stores `gl_Position = (0, 0, 0, 1)` before anything else and the native one does
not. That store comes from the IR `Prologue`, which the Maxwell frontend emits at the start of
every program and the SPIR-V backend turns into the default position for a vertex stage. NVN2
vertex programs write only the components they compute, so without it Z and W stay undefined and
every triangle lands outside the view - the scene draws, the draw counts match, and the pass ends
up holding just the clear colour.

The native program builder now emits the same prologue, with a test that a vertex program keeps it
through the whole pipeline.

Live on the restored baseline save, cold cache, `EDEN_NX2_NATIVE_STAGES=vertex,mesh`: 296 vertex
programs translate and the scene renders, matching the lowering's frame to within the temporal
noise (mean 5.1 per channel over the whole frame, 6.1 over the character). One difference is left
in that frame: the stone doorway in the Deku Tree is lit by the lowering and black with native
vertex, so at least one program still computes something different.

Vertex stays opt-in (`EDEN_NX2_NATIVE_STAGES=vertex,mesh`) until that difference is understood;
`EDEN_NX2_NATIVE` alone still covers compute and fragment.

## 2026-11-11 - The Deku Tree door: not a vertex program

Followed the door difference through the frame dumps of a baseline run and a vertex-native run at the
same save.

- The door's visible difference first appears in the 2560x1440 pass numbered 78 in the native run
  (79 in the baseline), whose two draws have the **same two shaders in both runs** - and both of
  those programs disassemble to the same body. So the door is not being drawn by a differently
  translated vertex program.
- The pass before it in that chain (43/44) is a specular-only target: black apart from highlights,
  and its door-region difference is 0.19 per channel. The shadow and depth passes match even closer.
- Every other pass in the frame matches within noise; the door region is the only place that
  separates the two runs (8.07 per channel in pass 78/79 against 0.19 or less everywhere else).

So the remaining difference is not in shader translation: it is in what that pass reads (a light,
shadow or texture product of an earlier pass, or a state that the native vertex programs change
indirectly). That is RenderDoc territory - pick the door's draw, inspect its bindings and the
texture it samples - rather than more SPIR-V comparison.

## 2026-11-11 - The door is real, and it is not the two programs I expected

Two baseline runs from the same save with a cleared cache both show the carved door panel; the
vertex-native run does not. So the difference is real and not temporal: the panel object is not
drawn (or lands off screen).

Where it first appears: with the passes matched by format, size and address, only four passes differ
at all - the 1280x720 scene pass, the 960x540 and 2560x1440 copies of it, and the 192x192 map
texture. Every pass before them matches. So the panel is lost at the 1280x720 scene pass, which the
dump lists with exactly two shaders.

Both of those were compared against the lowering and are equivalent: the vertex program's body
differs only in ids (same inputs, same `gl_Position` X/Y/Z stores, same `out_attr0`), and the
fragment program is the lowering's in both runs.

Skipping those two hashes with the new `EDEN_NX2_NATIVE_SKIP_HASH` did not bring the panel back, so
either the switch did not take effect in that run or the panel is drawn by a pipeline the pass does
not list. That is the next thing to check - rerun with the skip, and confirm in the log that the two
hashes no longer report `Native frontend ... translated`, before drawing any conclusion from it.

`EDEN_NX2_NATIVE_SKIP_HASH=<hash>,...` is new: those stage programs are left to the lowering, so a
single program can be taken out of the native frontend for an A/B.

## 2026-11-11 - The Deku Tree door is one vertex program

`EDEN_NX2_NATIVE_SKIP_HASH` works: in a run that skips `da8f4664b4f42327` and `4b08664c759deb48`
the log reports no `Native frontend: 0x... translated` line for either, and the panel is still
missing. So the pass's two listed shaders are not the cause, and the earlier result that "skipping
did not restore the panel" was genuine, not a broken switch.

Mesh programs are not the cause either: with `EDEN_NX2_NATIVE_STAGES=vertex` (mesh left to the
lowering) the panel is still missing.

Skipping all 164 vertex programs the native frontend translated brings the panel back, so the door
region is a usable oracle: 2.1-4.6 with the panel, 13-15 without it (crop 200,20-520,320 of the
1280x720 screenshot, away from the fairy). Bisecting that list eight rounds narrowed it to one
program: `40e7bd9af74fe01c`. Skipping only that hash restores the panel.

The program is a vertex shader of the 2560x1440 scene pass (its shader list carries it), 201
instructions: 86 FFMA, 53 FMUL, 18 FADD, 12 `LDC` with a register base and 64-bit width out of bank
6, 8 `ALD`, 8 `AST`, and one each of `LDG`, `ISETP` immediate, `SHF` immediate, `SEL` and two `MUFU`.
Its lowered and native SPIR-V store the same things (six generics and `gl_Position` as the sum of two
products), so the transcription is structurally right and the difference is in the values.

Two read differences stand out between the two modules for that program: the native path reads
constant word 69 in addition to 68, and repeats a `c0[34]`/`c0[70]` pair six times where the
lowering reads it once. That is where to look next.

A note on earlier pass comparisons: passes were matched by (format, size, address), which silently
drops a pass when a run gives it a different address. The first differing pass therefore looked like
the 1280x720 one; the 2560x1440 scene pass that lists `40e7bd9af74fe01c` is a better candidate.

## 2026-11-11 - A native address the storage buffer pass cannot see

The one program behind the door, `40e7bd9af74fe01c`, emits its only memory read as a runtime pointer
table access: its SPIR-V reads the pointer words out of the constant buffer and routes the access at
run time, while the lowering's module for the same program reads one storage buffer. Both are
correct in principle, so the first suspect was the address itself.

`ir_opt/global_memory_to_storage_buffer_pass.cpp` reads a global access's address through
`TrackLowAddress`: one `IAdd64` whose second operand is an immediate, then `PackUint2x32`, then
`CompositeConstructU32x2`. The Maxwell lowering always produces that shape - it materialises the
base-plus-offset sum into a register pair with `IADD3` before the load, so the address is
`PackUint2x32(pair) + Imm64`. The native frontend instead summed packed 64-bit values and expressed
the immediate as `PackUint2x32(CompositeConstruct(Imm32, Imm32))`, so the pass saw no immediate and
gave up on the whole access.

`src/tests/video_core/global_address_tracking.cpp` now builds each shape and asserts the pass keeps
the load on a storage buffer; the two packed shapes fail there before the change. The native emitters
now add a register offset word by word - `IAdd32` for the low word, the carry from a 64-bit sum added
to the high word - and pass the immediate as a real `Imm64`. The program's module then reads
`ssbo0` like the lowering's does.

That did not restore the door panel, so the program has a second difference. It is still the only
program that matters: with it skipped, every other vertex program native, the panel is present.
