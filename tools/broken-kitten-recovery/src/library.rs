//! The folder of images: downloading into it, and checking everything in it against the official keys.
//! Layout: <dir>/recovery/{recovery.manifest, modules.manifest, legal.txt, <image files>}: the same paths the device asks for.

use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

use crate::image::{self, CHIP_ANY, CHIP_ESP32S3};
use crate::keys::{Key, KEYS};
use crate::manifest::{self, Entry};

const MAX_FILE: u64 = 8 * 1024 * 1024;
pub const MANIFESTS: [&str; 2] = ["recovery.manifest", "modules.manifest"];
pub const DRIVERS_MANIFEST: &str = "drivers.manifest";

pub fn recovery_dir(dir: &Path) -> PathBuf { dir.join("recovery") }

fn hex(b: &[u8]) -> String { b.iter().map(|x| format!("{:02x}", x)).collect() }

pub fn sha256_hex(b: &[u8]) -> String { hex(&Sha256::digest(b)) }

/// Does this entry apply to the board and chip we serve?
fn applies(e: &Entry, board: &str) -> bool {
    (e.board == board || e.board == "any") && (e.chip.is_empty() || e.chip == "esp32s3" || e.chip == "any")
}

/// One file against its manifest entry and the keys: the length and SHA-256 the manifest promised, then the image's own
/// payload hash and signature, then that the payload hash is the one the manifest named.
pub fn check_file(e: &Entry, data: &[u8], keys: &[Key]) -> Result<image::Header, String> {
    if data.len() != e.size {
        return Err(format!("{}: {} bytes, the manifest says {}", e.file, data.len(), e.size));
    }
    if sha256_hex(data) != e.sha256 {
        return Err(format!("{}: does not match the hash in the manifest", e.file));
    }
    let chip = if e.chip == "any" { CHIP_ANY } else { CHIP_ESP32S3 };
    let h = image::verify(data, chip, keys).map_err(|err| format!("{}: {}", e.file, err))?;
    if !e.payload_sha256.is_empty() && hex(&h.payload_sha256) != e.payload_sha256 {
        return Err(format!("{}: its contents are not the ones the manifest named", e.file));
    }
    Ok(h)
}

/// Downloads one URL into memory, at most MAX_FILE bytes.
pub fn get(url: &str) -> Result<Vec<u8>, String> {
    let resp = ureq::get(url).timeout(std::time::Duration::from_secs(60)).call().map_err(|e| format!("{}: {}", url, e))?;
    let mut buf = Vec::new();
    resp.into_reader().take(MAX_FILE + 1).read_to_end(&mut buf).map_err(|e| format!("{}: {}", url, e))?;
    if buf.len() as u64 > MAX_FILE {
        return Err(format!("{}: larger than {} bytes", url, MAX_FILE));
    }
    Ok(buf)
}

fn write_atomic(path: &Path, data: &[u8]) -> Result<(), String> {
    let tmp = path.with_extension("part");
    fs::write(&tmp, data).map_err(|e| format!("{}: {}", tmp.display(), e))?;
    fs::rename(&tmp, path).map_err(|e| format!("{}: {}", path.display(), e))
}

/// Fetches `<base>/<manifest_name>` and every file it lists for this board. Files are written only after they pass; the
/// manifest is written last, only if all of them did. Returns how many images were fetched.
pub fn fetch(base: &str, manifest_name: &str, dir: &Path, board: &str, keys: &[Key], log: &mut dyn FnMut(&str)) -> Result<usize, String> {
    let rdir = recovery_dir(dir);
    fs::create_dir_all(&rdir).map_err(|e| format!("{}: {}", rdir.display(), e))?;
    let base = base.trim_end_matches('/');
    log(&format!("fetching {}/{}", base, manifest_name));
    let text = get(&format!("{}/{}", base, manifest_name))?;
    let text = String::from_utf8(text).map_err(|_| format!("{}: not text", manifest_name))?;
    let entries: Vec<Entry> = manifest::parse(&text).into_iter().filter(|e| applies(e, board)).collect();
    if entries.is_empty() {
        return Err(format!("{} has nothing for the board \"{}\"", manifest_name, board));
    }
    let mut staged: Vec<(PathBuf, Vec<u8>)> = Vec::new();
    for e in &entries {
        let data = get(&format!("{}/{}", base, e.file))?;
        let h = check_file(e, &data, keys)?;
        log(&format!("  ok  {:<10} {:<8} {} bytes, signed by key {}", e.component, e.version, data.len(), h.key_id));
        staged.push((rdir.join(&e.file), data));
    }
    for (path, data) in &staged {
        write_atomic(path, data)?;
    }
    write_atomic(&rdir.join(manifest_name), text.as_bytes())?;
    Ok(entries.len())
}

/// legal.txt is plain text and not signed: best effort, never stops anything.
pub fn fetch_legal(base: &str, dir: &Path) {
    if let Ok(t) = get(&format!("{}/legal.txt", base.trim_end_matches('/'))) {
        let _ = write_atomic(&recovery_dir(dir).join("legal.txt"), &t);
    }
}

/// Every image in the folder re-checked from disk against its manifest and the keys. On success: the bytes of everything
/// that may be served, by the name the device will ask for. Any failure is an error and nothing is served.
pub fn check_dir(dir: &Path, board: &str, keys: &[Key], dev: bool, log: &mut dyn FnMut(&str)) -> Result<BTreeMap<String, Vec<u8>>, String> {
    let rdir = recovery_dir(dir);
    let mut files = BTreeMap::new();
    let mut images = 0;
    if !rdir.join("recovery.manifest").exists() {
        return Err(format!("{} has no recovery.manifest: fetch first", rdir.display()));
    }
    for m in MANIFESTS {
        let p = rdir.join(m);
        let Ok(bytes) = fs::read(&p) else { continue };
        let text = String::from_utf8(bytes.clone()).map_err(|_| format!("{}: not text", m))?;
        let entries: Vec<Entry> = manifest::parse(&text).into_iter().filter(|e| applies(e, board)).collect();
        if entries.is_empty() {
            return Err(format!("{} has nothing for the board \"{}\"", m, board));
        }
        for e in &entries {
            let data = fs::read(rdir.join(&e.file)).map_err(|err| format!("{}: {}", e.file, err))?;
            check_file(e, &data, keys)?;
            log(&format!("  verified  {}", e.file));
            files.insert(e.file.clone(), data);
            images += 1;
        }
        files.insert(m.to_string(), bytes);
    }
    if dev {
        if let Ok(bytes) = fs::read(rdir.join(DRIVERS_MANIFEST)) {
            let text = String::from_utf8(bytes.clone()).map_err(|_| format!("{}: not text", DRIVERS_MANIFEST))?;
            for e in manifest::parse(&text).into_iter().filter(|e| applies(e, board)) {
                let data = fs::read(rdir.join(&e.file)).map_err(|err| format!("{}: {}", e.file, err))?;
                let trust = check_driver(&e, &data, keys)?;
                log(&format!("  driver    {}  {}", e.file, trust));
                files.insert(e.file.clone(), data);
                images += 1;
            }
            files.insert(DRIVERS_MANIFEST.to_string(), bytes);
        }
    }
    if let Ok(t) = fs::read(rdir.join("legal.txt")) {
        files.insert("legal.txt".to_string(), t);
    }
    log(&format!("{} image(s) checked against the official keys", images));
    Ok(files)
}

/// A driver for developer mode: the file against its manifest entry, then a well-formed driver module with an intact payload.
/// Says whether it is signed by an official key or unsigned (which a device loads only in developer mode).
pub fn check_driver(e: &Entry, data: &[u8], keys: &[Key]) -> Result<String, String> {
    if data.len() != e.size || sha256_hex(data) != e.sha256 {
        return Err(format!("{}: does not match the drivers manifest", e.file));
    }
    let (h, trust) = image::inspect_driver(data, CHIP_ESP32S3, keys).map_err(|err| format!("{}: {}", e.file, err))?;
    Ok(match trust {
        image::Trust::Signed(id) => format!("{} {}, signed by key {}", h.name, h.version, id),
        image::Trust::Unsigned => format!("{} {}, UNSIGNED: only a device in developer mode will load it", h.name, h.version),
    })
}

/// Adds (or replaces) a driver in <dir>/recovery/drivers.manifest and copies the file next to it, after checking it.
pub fn add_driver(dir: &Path, file: &Path, board: &str, compatible: &str, buses: &str, catcall: &str, keys: &[Key]) -> Result<String, String> {
    let data = fs::read(file).map_err(|e| format!("{}: {}", file.display(), e))?;
    let (h, trust) = image::inspect_driver(&data, CHIP_ESP32S3, keys).map_err(|e| format!("{}: {}", file.display(), e))?;
    let rdir = recovery_dir(dir);
    fs::create_dir_all(&rdir).map_err(|e| format!("{}: {}", rdir.display(), e))?;
    let name: String = h.name.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '_' || *c == '-').collect();
    if name.is_empty() {
        return Err("the driver has no usable name in its header".into());
    }
    let fname = format!("{}-{}.cat", name, board);
    let key = match trust { image::Trust::Signed(_) => "developer", image::Trust::Unsigned => "none" };
    let stanza = format!(
        "component={}\ntype=driver\nversion={}\nchip=esp32s3\nboard={}\nfile={}\nsize={}\nsha256={}\nkey={}\ncompatible={}\nbuses={}\ncatcall={}\n",
        name, h.version, board, fname, data.len(), sha256_hex(&data), key, compatible, buses, catcall);
    let mpath = rdir.join(DRIVERS_MANIFEST);
    let old = fs::read_to_string(&mpath).unwrap_or_default();
    let kept: Vec<&str> = old.split("\n\n").filter(|b| !b.trim().is_empty() && !b.contains(&format!("component={}\n", name))).collect();
    let mut out = String::new();
    for b in kept { out.push_str(b.trim_end()); out.push_str("\n\n"); }
    out.push_str(&stanza);
    write_atomic(&rdir.join(&fname), &data)?;
    write_atomic(&mpath, out.as_bytes())?;
    Ok(format!("{} {} added ({})", name, h.version, match trust { image::Trust::Signed(id) => format!("signed by key {}", id), image::Trust::Unsigned => "unsigned".to_string() }))
}

pub fn official_keys() -> &'static [Key] { KEYS }

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn entry_filter_follows_board_and_chip() {
        let e = |board: &str, chip: &str| Entry {
            component: "k".into(), board: board.into(), chip: chip.into(), version: "1".into(), file: "k.cat".into(),
            size: 1, sha256: "0".repeat(64), payload_sha256: String::new(),
            kind: String::new(), key: String::new(), compatible: String::new(), buses: String::new(), catcall: String::new(),
        };
        assert!(applies(&e("tdeck_plus", "esp32s3"), "tdeck_plus"));
        assert!(applies(&e("any", "any"), "tdeck_plus"));
        assert!(!applies(&e("waveshare154", "esp32s3"), "tdeck_plus"));
        assert!(!applies(&e("tdeck_plus", "esp32"), "tdeck_plus"));
    }

    #[test]
    fn a_wrong_size_or_hash_is_refused_before_the_signature_is_looked_at() {
        let e = Entry { component: "k".into(), board: "any".into(), chip: "esp32s3".into(), version: "1".into(),
                        file: "k.cat".into(), size: 4, sha256: sha256_hex(b"abcd"), payload_sha256: String::new(),
                        kind: String::new(), key: String::new(), compatible: String::new(), buses: String::new(), catcall: String::new() };
        assert!(check_file(&e, b"abc", &[]).unwrap_err().contains("manifest says 4"));
        assert!(check_file(&e, b"abce", &[]).unwrap_err().contains("hash in the manifest"));
    }
}
