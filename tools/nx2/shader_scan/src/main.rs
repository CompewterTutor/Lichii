use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use anyhow::{bail, Result};
use memchr::memmem;
use sha2::{Digest, Sha256};

const MAGIC_BNSH: &[u8; 8] = b"BNSH\0\0\0\0";
const MAGIC_ZSTD: &[u8; 4] = &[0x28, 0xb5, 0x2f, 0xfd];
const CTL_MAGIC: &[u8; 4] = &[0x34, 0x12, 0x76, 0x98];

struct FileRec {
    name: String,
    extent: u64,
    extent_size: u64,
    codec: u8,
}

fn u32le(d: &[u8], o: usize) -> Option<u32> {
    d.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()))
}
fn u64le(d: &[u8], o: usize) -> Option<u64> {
    d.get(o..o + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap()))
}
fn i32le(d: &[u8], o: usize) -> Option<i32> {
    d.get(o..o + 4).map(|b| i32::from_le_bytes(b.try_into().unwrap()))
}

fn read_nup(path: &Path) -> Result<Vec<FileRec>> {
    let mut f = File::open(path)?;
    let mut sb = [0u8; 0x80];
    f.read_exact(&mut sb)?;
    if &sb[0..4] != b"NUP\x01" {
        bail!("not a NUP");
    }
    let toc_offset = u64le(&sb, 8).unwrap();
    let toc_size = u64le(&sb, 16).unwrap() as usize;
    f.seek(SeekFrom::Start(toc_offset))?;
    let mut toc = vec![0u8; toc_size];
    f.read_exact(&mut toc)?;
    if u32le(&toc, 0) != Some(0x0143_4f54) {
        bail!("bad TOC magic");
    }
    let pkg_count = u32le(&toc, 4).unwrap() as usize;
    let dir_count = u64le(&toc, 8).unwrap() as usize;
    let file_count = u64le(&toc, 16).unwrap() as usize;
    let pool_offset = u64le(&toc, 24).unwrap() as usize;
    let mut pos = 0x30 + pkg_count * 0x40 + dir_count * 0x18;
    let name = |off: usize, len: usize| -> String {
        String::from_utf8_lossy(&toc[pool_offset + off..pool_offset + off + len]).into_owned()
    };
    let mut files = Vec::with_capacity(file_count);
    for _ in 0..file_count {
        let name_offset = u32le(&toc, pos).unwrap() as usize;
        let name_length = u32le(&toc, pos + 4).unwrap() as usize;
        let extent = u64le(&toc, pos + 0x10).unwrap();
        let extent_size = u64le(&toc, pos + 0x18).unwrap();
        let codec = toc[pos + 0x40];
        files.push(FileRec {
            name: name(name_offset, name_length),
            extent,
            extent_size,
            codec,
        });
        pos += 0x50;
    }
    Ok(files)
}

fn frame_dict_id(frame: &[u8]) -> Option<u32> {
    if frame.len() < 5 || &frame[0..4] != MAGIC_ZSTD {
        return None;
    }
    let fhd = frame[4];
    let dict_flag = fhd & 3;
    let single_segment = (fhd >> 5) & 1;
    let mut o = 5;
    if single_segment == 0 {
        o += 1; // window descriptor
    }
    match dict_flag {
        0 => None,
        1 => frame.get(o).map(|&b| b as u32),
        2 => frame.get(o..o + 2).map(|b| u16::from_le_bytes(b.try_into().unwrap()) as u32),
        3 => frame.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap())),
        _ => None,
    }
}

fn decompress(data: &[u8], dicts: &HashMap<u32, Vec<u8>>) -> Option<Vec<u8>> {
    if data.len() < 4 || &data[0..4] != MAGIC_ZSTD {
        return Some(data.to_vec());
    }
    let id = frame_dict_id(data);
    let empty: Vec<u8> = Vec::new();
    let dict = id.and_then(|id| dicts.get(&id)).unwrap_or(&empty);
    let mut decoder = zstd::stream::Decoder::with_dictionary(data, dict).ok()?;
    let mut out = Vec::new();
    std::io::copy(&mut decoder, &mut out).ok()?;
    Some(out)
}

fn nvn2_ctl(ctl: &[u8]) -> bool {
    ctl.len() >= 0x48 && &ctl[0..4] == CTL_MAGIC && &ctl[4..8] == &[0xc9, 0, 0, 0]
}

/// The control bytes that go into the key: an NVN2 section contributes its first 0x48 bytes with
/// +0x30 (its own length) zeroed, the same rule the host's keys_hash uses.
fn key_ctl(ctl: &[u8]) -> Option<Vec<u8>> {
    if nvn2_ctl(ctl) {
        let mut k = ctl[..0x48].to_vec();
        k[0x30..0x34].copy_from_slice(&[0, 0, 0, 0]);
        return Some(k);
    }
    if ctl.len() < 0x24 || &ctl[0..4] != CTL_MAGIC {
        return None;
    }
    let n = u32le(ctl, 0x20)? as usize;
    let n = if (0x760..0x880).contains(&n) { n } else { 0x880 };
    if ctl.len() < n {
        return None;
    }
    Some(ctl[..n].to_vec())
}

/// A graphics stage from SPH word 0 (bits 10..13), as the host reads an NVN2 code area.
fn sph_stage(code: &[u8]) -> Option<&'static str> {
    if code.len() < 4 {
        return None;
    }
    let w = u32le(code, 0)?;
    match (w >> 10) & 0xf {
        0 if w & 0x1f == 1 => Some("vs"),
        1 => Some("vs"),
        2 => Some("hs"),
        3 => Some("ds"),
        4 => Some("gs"),
        5 => Some("ps"),
        _ => None,
    }
}

fn nvn2_stage(ctl: &[u8], code: &[u8]) -> Option<&'static str> {
    if ctl.len() > 0x56 {
        match ctl[0x56] {
            5 => return Some("cs"),
            6 => return Some("ts"),
            7 => return Some("ms"),
            _ => {}
        }
    }
    sph_stage(code)
}

fn check(ctl: &[u8], code: &[u8]) -> bool {
    if ctl.len() < 0x24 || &ctl[0..4] != CTL_MAGIC {
        return false;
    }
    let len_field = if nvn2_ctl(ctl) { 0x44 } else { 0x704 };
    if ctl.len() < len_field + 4 {
        return false;
    }
    u32le(ctl, len_field) == Some(code.len() as u32)
}

type Keys = HashSet<(&'static str, String)>;

/// Extraction sink used by `--extract`. Kept in a thread-local so the container parsers stay
/// unchanged whether they only count or also write programs.
struct Programs {
    root: PathBuf,
    game: String,
    title_id: String,
    seen: HashSet<(&'static str, String, String)>,
    entries: Vec<String>,
}

thread_local! {
    static STORE: std::cell::RefCell<Option<Programs>> = const { std::cell::RefCell::new(None) };
    static LABEL: std::cell::RefCell<String> = const { std::cell::RefCell::new(String::new()) };
}

fn long_stage(stage: &str) -> &'static str {
    match stage {
        "vs" => "vertex",
        "hs" => "tess-control",
        "ds" => "tess-eval",
        "gs" => "geometry",
        "ps" => "fragment",
        "cs" => "compute",
        "ts" => "task",
        "ms" => "mesh",
        _ => "unknown",
    }
}

fn sha_hex(data: &[u8]) -> String {
    let mut hasher = Sha256::new();
    hasher.update(data);
    hasher.finalize().iter().map(|b| format!("{:02x}", b)).collect()
}

fn json_escape(value: &str) -> String {
    value.replace('\\', "\\\\").replace('"', "\\\"")
}

impl Programs {
    fn push(&mut self, stage: &'static str, code: &[u8], control: &[u8]) {
        let stage_long = long_stage(stage);
        let code_sha = sha_hex(code);
        let control_sha = sha_hex(control);
        if !self.seen.insert((stage_long, code_sha.clone(), control_sha.clone())) {
            return;
        }
        let programs = self.root.join("programs").join(&self.title_id);
        let controls = self.root.join("controls");
        let _ = std::fs::create_dir_all(&programs);
        let _ = std::fs::create_dir_all(&controls);
        let code_rel = format!("programs/{}/{}-{}.bin", self.title_id, code_sha, stage_long);
        let control_rel = format!("controls/{}.bin", control_sha);
        let _ = std::fs::write(self.root.join(&code_rel), code);
        let _ = std::fs::write(self.root.join(&control_rel), control);
        let format = match stage_long {
            "vertex" | "tess-control" | "tess-eval" | "geometry" | "fragment" => "\"native-header\"",
            _ => "null",
        };
        self.entries.push(format!(
            "{{\"game\":\"{}\",\"title_id\":\"{}\",\"stage\":\"{}\",\"code\":\"{}\",\"control\":\"{}\",\
\"code_sha256\":\"{}\",\"control_sha256\":\"{}\",\"format\":{},\
\"context\":\"archive header only; runtime resources unavailable\",\"origins\":[{{\"source\":\"{}\"}}]}}",
            json_escape(&self.game),
            self.title_id,
            stage_long,
            code_rel,
            control_rel,
            code_sha,
            control_sha,
            format,
            json_escape(&LABEL.with(|label| label.borrow().clone()))
        ));
    }
}


fn add(keys: &mut Keys, ctl: &[u8], code: &[u8], stage: &'static str) {
    let Some(k) = key_ctl(ctl) else { return };
    let mut hasher = Sha256::new();
    hasher.update(&k);
    hasher.update(code);
    let digest = hasher.finalize();
    let key: String = digest[..8].iter().map(|b| format!("{:02x}", b)).collect();
    if keys.insert((stage, key)) {
        STORE.with(|store| {
            if let Some(programs) = store.borrow_mut().as_mut() {
                programs.push(stage, code, ctl);
            }
        });
    }
}

fn parse_bnsh(bnsh: &[u8], keys: &mut Keys, records: &mut usize) {
    if bnsh.len() < 0x98 || &bnsh[0..8] != MAGIC_BNSH || &bnsh[0x60..0x64] != b"grsc" {
        return;
    }
    let size = u32le(bnsh, 0x1c).unwrap_or(0) as usize;
    if size < 0x98 || size > bnsh.len() {
        return;
    }
    let d = &bnsh[..size];
    if u32le(d, 0x70).map(|v| v as u16) != Some(4) || u32le(d, 0x72).map(|v| v as u16) != Some(200) {
        return;
    }
    let count = u32le(d, 0x7c).unwrap_or(0) as usize;
    let variations = u64le(d, 0x80).unwrap_or(0) as usize;
    if count == 0 || count > 65536 || variations == 0 || variations + count * 0x40 > d.len() {
        return;
    }
    let stages = ["vs", "hs", "ds", "gs", "ps", "cs"];
    for v in 0..count {
        let program = u64le(d, variations + v * 0x40 + 0x10).unwrap_or(0) as usize;
        if program == 0 || program + 0x50 > d.len() {
            continue;
        }
        for (index, stage) in stages.iter().enumerate() {
            let record = u64le(d, program + 8 + index * 8).unwrap_or(0) as usize;
            if record == 0 || record + 0x20 > d.len() {
                continue;
            }
            let control_offset = u64le(d, record + 8).unwrap_or(0) as usize;
            let code_offset = u64le(d, record + 16).unwrap_or(0) as usize;
            let code_size = u32le(d, record + 24).unwrap_or(0) as usize;
            let control_size = u32le(d, record + 28).unwrap_or(0) as usize;
            if code_offset == 0 || code_size == 0 || code_offset + code_size > d.len() {
                continue;
            }
            if control_size != 0 && control_offset + control_size > d.len() {
                continue;
            }
            let control = &d[control_offset..control_offset + control_size];
            let code = &d[code_offset..code_offset + code_size];
            add(keys, control, code, stage);
            *records += 1;
        }
        // NVN2's task and mesh stages follow at +0x40 and +0x48 of the program.
        for (offset, stage, stage_id) in [(0x40usize, "ts", 6u8), (0x48, "ms", 7)] {
            let record = u64le(d, program + offset).unwrap_or(0) as usize;
            if record == 0 || record + 0x20 > d.len() {
                continue;
            }
            let control_offset = u64le(d, record + 8).unwrap_or(0) as usize;
            let code_offset = u64le(d, record + 16).unwrap_or(0) as usize;
            let code_size = u32le(d, record + 24).unwrap_or(0) as usize;
            let control_size = u32le(d, record + 28).unwrap_or(0) as usize;
            if code_offset == 0 || code_size == 0 || code_offset + code_size > d.len() {
                continue;
            }
            if control_size < 0x57 || control_offset + control_size > d.len() {
                continue;
            }
            let control = &d[control_offset..control_offset + control_size];
            if control[0x56] != stage_id {
                continue;
            }
            let code = &d[code_offset..code_offset + code_size];
            add(keys, control, code, stage);
            *records += 1;
        }
    }
}

/// agl's BAHS (sharcb) container: a code region after the header, and per-shader 16-byte entries
/// that name a control section, its offset into the code region and the code length.
fn parse_bahs(d: &[u8], keys: &mut Keys, records: &mut usize, stats: &mut HashMap<&'static str, usize>) {
    if d.len() < 0x20 || &d[0..4] != b"BAHS" {
        return;
    }
    let rsz = u32le(d, 0x18).unwrap_or(0) as usize;
    let region = u32le(d, 0x1c).unwrap_or(0) as usize;
    if rsz == 0 || region + rsz > d.len() || region < 16 {
        return;
    }
    for a in (0..region - 16).step_by(4) {
        let Some(off) = u32le(d, a) else { continue };
        let z = u32le(d, a + 4).unwrap_or(1);
        let csz = u32le(d, a + 8).unwrap_or(0) as usize;
        let rel = i32le(d, a + 12).unwrap_or(-1);
        if rel < 0 {
            continue;
        }
        let c = match (a as i64).checked_add(rel as i64) {
            Some(c) if c >= 0 => c as usize,
            _ => continue,
        };
        let nvn2 = (0x48..0x10000).contains(&csz)
            && c + csz <= region
            && nvn2_ctl(&d[c..c + 0x48]);
        let legacy = csz == 0x880 || csz == 0x88c || (0x760..0x880).contains(&csz);
        if z != 0 || !(nvn2 || legacy) || off as usize % 0x100 != 0 {
            continue;
        }
        if c + csz > region || &d[c..c + 4] != CTL_MAGIC {
            continue;
        }
        let len_field = if nvn2 { 0x44 } else { 0x704 };
        let Some(n) = u32le(d, c + len_field) else { continue };
        let n = n as usize;
        if n == 0 || off as usize + n > rsz || region + off as usize + n > d.len() {
            continue;
        }
        let code = &d[region + off as usize..region + off as usize + n];
        let ctl = &d[c..c + csz];
        let Some(stage) = (if nvn2 { nvn2_stage(ctl, code) } else { sph_stage(code) }) else {
            continue;
        };
        if !check(ctl, code) {
            continue;
        }
        add(keys, ctl, code, stage);
        *records += 1;
        *stats.entry(stage).or_default() += 1;
    }
}

/// agl's NDSR upscaler pack: 16-byte records that name a control section, a code area and lengths.
fn parse_ndsr(d: &[u8], keys: &mut Keys, records: &mut usize, stats: &mut HashMap<&'static str, usize>) {
    if d.len() < 0x20 || &d[0..4] != b"NDSR" {
        return;
    }
    let pointing = |o: usize| -> usize {
        let mut hits = 0;
        for i in 0..16 {
            let at = o + 16 * i;
            if at + 16 > d.len() {
                break;
            }
            let c = u32le(d, at).unwrap_or(0) as usize;
            if c + 0x48 <= d.len() && nvn2_ctl(&d[c..c + 0x48]) {
                hits += 1;
            }
        }
        hits
    };
    let mut o = if pointing(0x14) > pointing(0x10) { 0x14 } else { 0x10 };
    while o + 16 <= d.len() {
        let c = u32le(d, o).unwrap_or(0) as usize;
        let cl = u32le(d, o + 4).unwrap_or(0);
        let k = u32le(d, o + 8).unwrap_or(0) as usize;
        let fourth = u32le(d, o + 12).unwrap_or(0);
        if c == 0 && cl == 0 && k == 0 && fourth == 0 {
            break;
        }
        if !(c + 0x48 <= d.len() && nvn2_ctl(&d[c..c + 0x48])) {
            o += 8;
            continue;
        }
        o += 16;
        let ctl_len = ((cl >> 4) as usize).max(0x48);
        if c + ctl_len > d.len() {
            continue;
        }
        let ctl = &d[c..c + ctl_len];
        let Some(n) = u32le(ctl, 0x44) else { continue };
        let n = n as usize;
        if n == 0 || k + n > d.len() {
            continue;
        }
        let code = &d[k..k + n];
        let Some(stage) = nvn2_stage(ctl, code) else { continue };
        if !check(ctl, code) {
            continue;
        }
        add(keys, ctl, code, stage);
        *records += 1;
        *stats.entry(stage).or_default() += 1;
    }
}

/// nn::ndve's movie enhancement file: an NDSR pack plus roulibr compute networks whose control
/// sections name their own length and their code area's, the code right behind the control.
fn parse_ndve(d: &[u8], keys: &mut Keys, records: &mut usize, stats: &mut HashMap<&'static str, usize>) {
    if d.len() < 0x18 || &d[0..4] != b"NDVE" {
        return;
    }
    let count = u32le(d, 0x14).unwrap_or(0) as usize;
    let pack = 0x18 + 8 * count;
    let nets = pack + u32le(d, 0xc).unwrap_or(0) as usize;
    if pack < nets && nets <= d.len() {
        parse_ndsr(&d[pack..nets], keys, records, stats);
    }
    if nets > d.len() {
        return;
    }
    let mut c = memmem::find(&d[nets..], CTL_MAGIC).map(|p| p + nets);
    while let Some(at) = c {
        if at % 4 == 0 && at + 0x48 <= d.len() && nvn2_ctl(&d[at..at + 0x48]) {
            let own = u32le(d, at + 0x30).unwrap_or(0) as usize;
            let n = u32le(d, at + 0x44).unwrap_or(0) as usize;
            if own >= 0x48 && at + own + n <= d.len() && n > 0 {
                let ctl = &d[at..at + own];
                let code = &d[at + own..at + own + n];
                if let Some(stage) = nvn2_stage(ctl, code) {
                    if check(ctl, code) {
                        add(keys, ctl, code, stage);
                        *records += 1;
                        *stats.entry(stage).or_default() += 1;
                    }
                }
            }
        }
        c = memmem::find(&d[at + 4..], CTL_MAGIC).map(|p| p + at + 4);
    }
}

/// The members of a SARC archive: header, SFAT entries, SFNT names, then the data.
fn sarc_members<'a>(d: &'a [u8]) -> Vec<(String, &'a [u8])> {
    if d.len() < 0x14 || &d[0..4] != b"SARC" {
        return Vec::new();
    }
    let hl = d.get(4..6).map(|b| u16::from_le_bytes(b.try_into().unwrap())).unwrap_or(0) as usize;
    let doff = u32le(d, 12).unwrap_or(0) as usize;
    let n = d.get(hl + 6..hl + 8).map(|b| u16::from_le_bytes(b.try_into().unwrap())).unwrap_or(0) as usize;
    if hl + 12 + 16 * n > d.len() || doff > d.len() {
        return Vec::new();
    }
    let names = hl + 12 + 16 * n + 8;
    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        let at = hl + 12 + 16 * i;
        let attr = u32le(d, at + 4).unwrap_or(0);
        let s = u32le(d, at + 8).unwrap_or(0) as usize;
        let e = u32le(d, at + 12).unwrap_or(0) as usize;
        if s > e || doff + e > d.len() {
            continue;
        }
        let name = if attr >> 24 != 0 {
            let start = names + ((attr & 0x00ff_ffff) as usize) * 4;
            match d.get(start..).and_then(|tail| memchr::memchr(0, tail).map(|end| &tail[..end])) {
                Some(raw) => String::from_utf8_lossy(raw).into_owned(),
                None => format!("#{i}"),
            }
        } else {
            format!("#{i}")
        };
        out.push((name, &d[doff + s..doff + e]));
    }
    out
}

fn scan_blob(d: &[u8], keys: &mut Keys, records: &mut usize, stats: &mut HashMap<&'static str, usize>, found_blobs: &mut usize, sources: &mut HashMap<String, usize>, label: &str) {
    LABEL.with(|l| *l.borrow_mut() = label.to_string());
    if &d[0..4.min(d.len())] == b"BAHS" {
        parse_bahs(d, keys, records, stats);
    }
    if &d[0..4.min(d.len())] == b"NDSR" {
        parse_ndsr(d, keys, records, stats);
    }
    if &d[0..4.min(d.len())] == b"NDVE" {
        parse_ndve(d, keys, records, stats);
    }
    for m in memmem::find_iter(d, MAGIC_BNSH) {
        if m + 0x20 > d.len() {
            continue;
        }
        let size = u32le(d, m + 0x1c).unwrap_or(0) as usize;
        if size < 0x98 || m + size > d.len() {
            continue;
        }
        parse_bnsh(&d[m..m + size], keys, records);
        *found_blobs += 1;
        *sources.entry(label.to_string()).or_default() += 1;
    }
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() >= 2 && args[1] == "--dump" {
        // --dump <game.nup> <dictdir> <name substring> <out file>
        if args.len() < 6 {
            bail!("usage: shader-scan --dump <game.nup> <dictdir> <name substring> <out>");
        }
        let mut dicts = HashMap::new();
        let mut exact: HashMap<u32, Vec<u8>> = HashMap::new();
        for entry in std::fs::read_dir(&args[3])? {
            let path = entry?.path();
            let name = path.file_name().unwrap().to_string_lossy().into_owned();
            if let Some(id) = name.strip_prefix("dict-").and_then(|s| s.split('-').next()) {
                if let Ok(id) = id.parse::<u32>() {
                    if let Ok(bytes) = std::fs::read(&path) {
                        if name.contains("-exact") {
                            exact.insert(id, bytes);
                        } else {
                            dicts.insert(id, bytes);
                        }
                    }
                }
            }
        }
        for (id, bytes) in exact {
            dicts.insert(id, bytes);
        }
        let files = read_nup(Path::new(&args[2]))?;
        let mut f = File::open(&args[2])?;
        for rec in &files {
            if !rec.name.contains(&args[4]) || rec.extent_size == 0 {
                continue;
            }
            let mut data = vec![0u8; rec.extent_size as usize];
            f.seek(SeekFrom::Start(rec.extent))?;
            f.read_exact(&mut data)?;
            if rec.codec == 1 {
                data = zstd::decode_all(&data[..])?;
            }
            let out = match decompress(&data, &dicts) {
                Some(out) => out,
                None => {
                    if data.len() >= 4 && &data[0..4] == MAGIC_ZSTD {
                        let id = frame_dict_id(&data);
                        let empty: Vec<u8> = Vec::new();
                        let dict = id.and_then(|i| dicts.get(&i)).unwrap_or(&empty);
                        let mut dec = zstd::stream::Decoder::with_dictionary(&data[..], dict)
                            .map_err(|e| anyhow::anyhow!("decoder {} for dict {id:?}: {e}", dict.len()))?;
                        let mut v = Vec::new();
                        std::io::copy(&mut dec, &mut v)
                            .map_err(|e| anyhow::anyhow!("decode {} for dict {id:?}: {e}", dict.len()))?;
                        v
                    } else {
                        Vec::new()
                    }
                }
            };
            std::fs::write(&args[5], &out)?;
            println!("wrote {} {} bytes (codec {})", rec.name, out.len(), rec.codec);
            return Ok(());
        }
        bail!("no file matching {}", args[4]);
    }
    if args.len() >= 2 && args[1] == "--extract" {
        // --extract <game.nup> <dictdir> <out dir> <title id> <game name> [max_mb]
        if args.len() < 7 {
            bail!("usage: shader-scan --extract <game.nup> <dictdir> <out> <title id> <game> [max_mb]");
        }
        let dicts = load_dicts(Path::new(&args[3]))?;
        let root = PathBuf::from(&args[4]);
        std::fs::create_dir_all(&root)?;
        STORE.with(|store| {
            *store.borrow_mut() = Some(Programs {
                root: root.clone(),
                game: args[6].clone(),
                title_id: args[5].clone(),
                seen: HashSet::new(),
                entries: Vec::new(),
            })
        });
        let max_mb: u64 = args.get(7).and_then(|s| s.parse().ok()).unwrap_or(128);
        let (keys, records, blobs, _sources, stages) = scan_nup(Path::new(&args[2]), &dicts, max_mb)?;
        let entries = STORE
            .with(|store| store.borrow_mut().take().map(|p| p.entries).unwrap_or_default());
        let manifest = format!(
            "{{\"schema_version\":1,\"archives\":[],\"programs\":[\n{}\n],\"errors\":[]}}\n",
            entries.join(",\n")
        );
        std::fs::write(root.join("manifest.json"), manifest)?;
        println!(
            "programs={} keys={} records={} blobs={} stages={:?}",
            entries_len(&root),
            keys.len(),
            records,
            blobs,
            {
                let mut s: Vec<_> = stages.into_iter().collect();
                s.sort();
                s
            }
        );
        return Ok(());
    }
    if args.len() < 3 {
        bail!("usage: shader-scan <game.nup> <dictdir> [max_mb] [keys_out.tsv]");
    }
    let nup = PathBuf::from(&args[1]);
    let max_mb: u64 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(128);
    let keys_out = args.get(4).cloned();
    let dicts = load_dicts(Path::new(&args[2]))?;
    eprintln!("dictionaries: {:?}", dicts.keys().collect::<Vec<_>>());
    let (keys, records, found_blobs, sources, stage_records) = scan_nup(&nup, &dicts, max_mb)?;
    let files = read_nup(&nup)?;
    println!("files={} blobs={} records={} unique_keys={}", files.len(), found_blobs, records, keys.len());
    let mut stages: HashMap<&str, usize> = HashMap::new();
    for (stage, _) in &keys {
        *stages.entry(stage).or_default() += 1;
    }
    let mut stage_list: Vec<_> = stages.into_iter().collect();
    stage_list.sort();
    println!("stages={:?}", stage_list);
    let mut sr: Vec<_> = stage_records.into_iter().collect();
    sr.sort();
    println!("new stage records (BAHS/NDSR/NDVE)={:?}", sr);
    let mut src_list: Vec<_> = sources.into_iter().collect();
    src_list.sort_by(|a, b| b.1.cmp(&a.1));
    println!("sources={}", src_list.len());
    for (name, count) in src_list.iter().take(10) {
        println!("  {count:6} {name}");
    }
    if let Some(path) = keys_out {
        let mut out = File::create(path)?;
        let mut sorted: Vec<_> = keys.iter().collect();
        sorted.sort();
        for (stage, key) in sorted {
            writeln!(out, "{stage}\t{key}")?;
        }
    }
    Ok(())
}

fn entries_len(root: &Path) -> usize {
    std::fs::read_to_string(root.join("manifest.json"))
        .map(|m| m.matches("\"stage\":").count())
        .unwrap_or(0)
}

fn load_dicts(dict_dir: &Path) -> Result<HashMap<u32, Vec<u8>>> {
    let mut dicts = HashMap::new();
    let mut exact: HashMap<u32, Vec<u8>> = HashMap::new();
    for entry in std::fs::read_dir(dict_dir)? {
        let path = entry?.path();
        let name = path.file_name().unwrap().to_string_lossy().into_owned();
        if let Some(id) = name.strip_prefix("dict-").and_then(|s| s.split('-').next()) {
            if let Ok(id) = id.parse::<u32>() {
                if let Ok(bytes) = std::fs::read(&path) {
                    if name.contains("-exact") {
                        exact.insert(id, bytes);
                    } else {
                        dicts.insert(id, bytes);
                    }
                }
            }
        }
    }
    for (id, bytes) in exact {
        dicts.insert(id, bytes);
    }
    Ok(dicts)
}

fn scan_nup(
    nup: &Path,
    dicts: &HashMap<u32, Vec<u8>>,
    max_mb: u64,
) -> Result<(Keys, usize, usize, HashMap<String, usize>, HashMap<&'static str, usize>)> {
    let files = read_nup(nup)?;
    let mut f = File::open(nup)?;
    let mut keys: Keys = HashSet::new();
    let mut records = 0usize;
    let mut found_blobs = 0usize;
    let mut sources: HashMap<String, usize> = HashMap::new();
    let mut stage_records: HashMap<&'static str, usize> = HashMap::new();
    let mut skipped = 0usize;
    for (index, rec) in files.iter().enumerate() {
        if index % 20000 == 0 {
            eprintln!("{index}/{} keys={} records={}", files.len(), keys.len(), records);
        }
        if rec.extent_size == 0 || rec.extent_size > max_mb * 1024 * 1024 {
            continue;
        }
        if rec.name.ends_with(".webm") {
            continue;
        }
        let mut data = vec![0u8; rec.extent_size as usize];
        f.seek(SeekFrom::Start(rec.extent))?;
        if f.read_exact(&mut data).is_err() {
            continue;
        }
        if rec.codec == 1 {
            match zstd::decode_all(&data[..]) {
                Ok(out) => data = out,
                Err(_) => continue,
            }
        }
        let Some(decoded) = decompress(&data, dicts) else {
            skipped += 1;
            continue;
        };
        let members = sarc_members(&decoded);
        scan_blob(&decoded, &mut keys, &mut records, &mut stage_records, &mut found_blobs, &mut sources, &rec.name);
        for (name, member) in members.iter() {
            let Some(m) = decompress(member, dicts) else { continue };
            let label = format!("{}/{}", rec.name, name);
            scan_blob(&m, &mut keys, &mut records, &mut stage_records, &mut found_blobs, &mut sources, &label);
            for (inner_name, inner) in sarc_members(&m).iter() {
                let Some(mm) = decompress(inner, dicts) else { continue };
                let label = format!("{}/{}/{}", rec.name, name, inner_name);
                scan_blob(&mm, &mut keys, &mut records, &mut stage_records, &mut found_blobs, &mut sources, &label);
            }
        }
    }
    if skipped != 0 {
        eprintln!("skipped {skipped} files: a zstd dictionary is missing");
    }
    Ok((keys, records, found_blobs, sources, stage_records))
}
