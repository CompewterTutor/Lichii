# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

### Changed

- The user directory is now `lichii` rather than `eden`. An existing Eden profile is offered for
  import on first launch; keeping the old data copies it and leaves the original untouched.
- The emulator is now on upstream's pipeline cache: pipelines are recorded and precached through the
  shader cache v2 described above, which replaces the earlier per-title key list and its tools.
  Existing caches keep working: this version still reads the two before it.

### Fixed

- A controller that stops reporting, as a DualSense does over Bluetooth, no longer leaves the last
  stick position applied: a deflection with no reports for a second is released to centre, so a
  character stops running in one direction instead of continuing until the pad reports again.

- Mesh programs with more than one exit built invalid SPIR-V: only one of the mesh set-outputs
  markers was rewritten and the others remained stores through a constant, which the driver rejects
  and can crash on. Every marker is rewritten now.
- Native shader translation (opt-in, EDEN_NX2_NATIVE) handles ALD, AST, IPA and ISBERD/ISBEWR,
  writes fragment color at the exit the way the hardware does, starts a vertex program with the
  default position the hardware's do, and swaps FMNMX/IMNMX results for the inverted-predicate form
  instead of negating the predicate.

### Added

- The shader cache records the guest environments a title's pipelines were translated from, in an
  append-only log beside the driver's own pipeline binaries, and builds the title's pipelines before
  play starts. Most mid-game compilation stalls are gone, and a later build reuses the driver's work
  instead of recompiling it. Records hold the guest code and the values its translation read, never
  SPIR-V.
- `tools/nx2/shader_scan` extracts the shader programs inside a NUP container (BNSH, BAHS, NDSR
  and NDVE), with `--extract` writing a manifest of programs and hashes for offline work.
- Debug server control tools, still opt-in through `EDEN_NX2_DEBUG_SERVER` and loopback only:
  `pause`, `resume`, `step` (present-granular), `input`, `input_sequence`, `input_record`,
  `input_playback`, guest and GPU memory read/write, `shaders`, `gpu_memory_map`, and screenshot
  shader toggles for A/B images without a relaunch.
- `tools/nx2/live_check.py` runs 21 live checks of those tools against a booted game.

### Changed

- `status` reports the pipeline cache counters (built, evicted, prefetched, prefetch failed) next to
  the boot and pacing fields.

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
