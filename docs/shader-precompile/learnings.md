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
