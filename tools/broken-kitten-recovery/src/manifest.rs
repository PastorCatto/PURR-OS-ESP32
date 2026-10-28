//! The release manifest (purr_manifest.h): stanzas of key=value lines separated by blank lines, `#` starts a comment.

use std::collections::HashMap;

#[derive(Debug, Clone)]
pub struct Entry {
    pub component: String,
    pub board: String,
    pub chip: String,
    pub version: String,
    pub file: String,
    pub size: usize,
    pub sha256: String,
    pub payload_sha256: String,
}

fn hex64(s: &str) -> bool { s.len() == 64 && s.bytes().all(|c| c.is_ascii_hexdigit()) }

/// A plain file name: no slash, no way out of the folder.
pub fn safe_name(n: &str) -> bool {
    !n.is_empty() && n.len() <= 80 && !n.contains("..")
        && n.bytes().next().map_or(false, |c| c.is_ascii_alphanumeric())
        && n.bytes().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'.' | b'-'))
}

/// Entries missing a required field, or with a malformed size, hash or file name, are dropped, as the device's parser does.
pub fn parse(text: &str) -> Vec<Entry> {
    let mut out = Vec::new();
    let mut cur: HashMap<String, String> = HashMap::new();
    let mut flush = |cur: &mut HashMap<String, String>| {
        if cur.contains_key("component") {
            let get = |k: &str| cur.get(k).cloned().unwrap_or_default();
            let size = get("size").parse::<usize>().unwrap_or(0);
            let e = Entry {
                component: get("component"), board: get("board"), chip: get("chip"), version: get("version"),
                file: get("file"), size, sha256: get("sha256").to_lowercase(),
                payload_sha256: get("payload_sha256").to_lowercase(),
            };
            if size > 0 && hex64(&e.sha256) && safe_name(&e.file) && (e.payload_sha256.is_empty() || hex64(&e.payload_sha256)) {
                out.push(e);
            }
        }
        cur.clear();
    };
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            flush(&mut cur);
        } else if !line.starts_with('#') {
            if let Some((k, v)) = line.split_once('=') {
                cur.insert(k.trim().to_string(), v.trim().to_string());
            }
        }
    }
    flush(&mut cur);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const H: &str = "1300b610e9897dd38c132d46b1dc1afe7ab7ef9b31121d0f05a14b152757b75b";

    #[test]
    fn parses_stanzas_and_drops_bad_ones() {
        let text = format!(
            "release=1.0.3\n\ncomponent=kernel\nboard=tdeck_plus\nchip=esp32s3\nfile=kernel_tdeck_plus.cat\nsize=10\nsha256={H}\npayload_sha256={H}\n\n\
             component=bad\nfile=../evil\nsize=10\nsha256={H}\n\ncomponent=short\nfile=x.cat\nsize=10\nsha256=abc\n"
        );
        let e = parse(&text);
        assert_eq!(e.len(), 1);
        assert_eq!(e[0].component, "kernel");
        assert_eq!(e[0].size, 10);
    }

    #[test]
    fn file_names_cannot_leave_the_folder() {
        assert!(safe_name("kernel_tdeck_plus.cat"));
        assert!(!safe_name("../x"));
        assert!(!safe_name("a/b"));
        assert!(!safe_name(".hidden"));
        assert!(!safe_name(""));
    }
}
