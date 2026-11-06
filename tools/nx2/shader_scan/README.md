# shader_scan

Walks a Switch 2 `NUP` package, decodes its RomFS containers and reports every native NVN2 shader
program it can pair with a control section. It reads the package directly: no emulator, no GPU and
no keys.

It covers the four containers that hold NVN2 programs:

| Container | Where | Stage source |
|---|---|---|
| `BNSH` (in `FSHA`/`bfsha`, `esetb`, `blarc`, main) | program slots `+0x08..+0x30`, task/mesh at `+0x40`/`+0x48` | slot, or control `+0x56` for task/mesh |
| `BAHS` (`agl`/`gsys`/`tera` `.sharcb`) | 16-byte entries per control section and code offset | code area SPH word 0, or control `+0x56` |
| `NDSR` (`agl_ndsr*.pck`) | 16-byte records | control `+0x56`, else SPH |
| `NDVE` (`Movie/Ndve/*.dat`) | NDSR pack plus compute networks | control `+0x56` |

Shaders are keyed exactly like the host's `keys_hash`: `sha256(key_ctl(control) + code)[:16]`, where
`key_ctl` is the first `0x48` bytes of an NVN2 control section with `+0x30` zeroed.

## Usage

```sh
cargo build --release

# Count every unique (stage, key) and write the list
shader-scan <game.nup> <dict dir> [max_mb] [keys.tsv]

# Write code/control blobs and a manifest that sweep_static_shaders.py accepts
shader-scan --extract <game.nup> <dict dir> <out dir> <title id> <game name> [max_mb]

# Dump one decompressed RomFS file for inspection
shader-scan --dump <game.nup> <dict dir> <name substring> <out file>
```

`<dict dir>` holds the zstd dictionaries the RomFS frames name, one file per ID:
`dict-<id>-exact.bin`. Recover them from a running game with the debug server's `scan_shaders`
magic scan (see `docs/nx2/debug-mcp.md`); a full 128 KiB slice per ID is what the frames
reference. Files whose dictionary is missing are skipped, and the run still reports the rest.

## Output

`--extract` writes:

```
<out>/manifest.json                       one entry per program
<out>/programs/<title id>/<code sha>-<stage>.bin
<out>/controls/<control sha>.bin
```

`manifest.json` follows the `extract_static_shaders.py` schema (`schema_version`, `programs`),
so `sweep_static_shaders.py` can lower and compile every program offline.
