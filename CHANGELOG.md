# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

### Fixed

- Mesh programs with more than one exit built invalid SPIR-V: only one of the mesh set-outputs
  markers was rewritten and the others remained stores through a constant, which the driver rejects
  and can crash on. Every marker is rewritten now.
- Native shader translation (opt-in, EDEN_NX2_NATIVE) handles ALD, IPA and ISBERD/ISBEWR,
  writes fragment color at the exit the way the hardware does, and swaps FMNMX/IMNMX results for
  the inverted-predicate form instead of negating the predicate.

### Added

- Per-title pipeline key lists: every pipeline the title builds is recorded as a row of shader
  hashes plus fixed pipeline state, and matching pipelines are built on background workers before
  the game asks for them, which removes most mid-game compilation stalls. Lists hold identifiers
  only, never shader code, SPIR-V, driver data or environment blobs.
- An LRU memory budget for the pipeline cache (1536 MB by default) replaces idle-frame mass
  eviction. Measured on The Legend of Zelda: Ocarina of Time: 0 evictions in four minutes against
  175 with the old policy. `EDEN_NX2_PIPELINE_EVICT=legacy` restores the old behaviour.
- `tools/nx2/shader_scan` extracts the shader programs inside a NUP container (BNSH, BAHS, NDSR
  and NDVE), with `--extract` writing a manifest of programs and hashes for offline key-list work.
- `tools/nx2/keylist_tool.py` inspects, merges and diffs key lists, with 12 unit tests.
- Debug server control tools, still opt-in through `EDEN_NX2_DEBUG_SERVER` and loopback only:
  `pause`, `resume`, `step` (present-granular), `input`, `input_sequence`, `input_record`,
  `input_playback`, guest and GPU memory read/write, `shaders`, `gpu_memory_map`, and screenshot
  shader toggles for A/B images without a relaunch.
- `tools/nx2/live_check.py` runs 21 live checks of those tools against a booted game.

### Changed

- `status` reports the pipeline cache counters (built, evicted, prefetched, prefetch failed) and the
  key-list row counts next to the boot and pacing fields.
- A session now writes the pipelines no shared list has into `keylist.contribution.bin` next to the
  list, so players collecting the same title can merge what they found instead of overwriting each
  other's list. `keylist_tool.py merge` already unions them, and merging twice changes nothing.
  `status.caches.keylist_contribution` says how many rows are waiting to be merged.

### Fixed

- Ocarina of Time's compute shaders that read a byte or a halfword through an indexed uniform load
  now translate instead of being skipped, which removes their fallback draws on the affected
  passes.
- Recorded input replays again: recordings store buttons as hex text, which the playback path fed
  through the button-name parser, so every replay failed with `unknown button`.
- `input_playback` reports how many events it replays instead of always `0`.
- A path the loader cannot open now logs `No file at <path>: check the path, it may differ in case
  or digits` instead of the generic `Failed to obtain loader`, which made a healthy ROM container
  look corrupt.

## [0.1.0]

- Initial release candidate
