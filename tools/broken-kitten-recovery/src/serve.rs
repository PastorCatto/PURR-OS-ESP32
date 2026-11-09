//! Serves the checked images to a device on the local network, and answers its discovery probe.
//!
//! Protocol (purrstrap dev serve, purr_lan_discover.h): the device broadcasts "PURR-REPO?" to UDP 47800 and hears back
//! "PURR-REPO! <name> <port> <id>"; the server also announces that line by itself every few seconds. Then it asks over HTTP for
//! /recovery/<file>. The id starts with "relay-" so a device can tell a verified relay from a developer's server.

use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use socket2::{Domain, Protocol, Socket, Type};

pub const DISCOVERY_PORT: u16 = 47800;

pub fn relay_id(manifest: &[u8]) -> String {
    format!("relay-{}", &crate::library::sha256_hex(manifest)[..8])
}

pub fn announce_line(name: &str, port: u16, id: &str) -> String {
    format!("PURR-REPO! {} {} {}", name.replace(' ', "_"), port, id)
}

/// What to send for one request line like "GET /recovery/x.cat HTTP/1.1": (status, body). HEAD gets the same minus the body.
pub fn respond(request_line: &str, files: &BTreeMap<String, Vec<u8>>) -> (u16, Option<Vec<u8>>) {
    let mut parts = request_line.split_whitespace();
    let (method, path) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
    if method != "GET" && method != "HEAD" {
        return (405, None);
    }
    let path = path.split('?').next().unwrap_or("");
    match path.strip_prefix("/recovery/").and_then(|n| files.get(n)) {
        Some(body) => (200, Some(body.clone())),
        None => (404, None),
    }
}

fn handle(mut s: TcpStream, files: &BTreeMap<String, Vec<u8>>, log: &(dyn Fn(String) + Sync)) {
    let _ = s.set_read_timeout(Some(Duration::from_secs(10)));
    let mut buf = [0u8; 4096];
    let mut n = 0;
    while n < buf.len() {
        match s.read(&mut buf[n..]) {
            Ok(0) | Err(_) => break,
            Ok(k) => {
                n += k;
                if buf[..n].windows(4).any(|w| w == b"\r\n\r\n") { break; }
            }
        }
    }
    let req = String::from_utf8_lossy(&buf[..n]);
    let line = req.lines().next().unwrap_or("");
    let (code, body) = respond(line, files);
    let peer = s.peer_addr().map(|a| a.to_string()).unwrap_or_default();
    log(format!("{} {} -> {}", peer, line, code));
    let reason = match code { 200 => "OK", 404 => "Not Found", _ => "Method Not Allowed" };
    let len = body.as_ref().map_or(0, |b| b.len());
    let head = format!("HTTP/1.1 {} {}\r\nContent-Type: application/octet-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", code, reason, len);
    let _ = s.write_all(head.as_bytes());
    if !line.starts_with("HEAD") {
        if let Some(b) = body { let _ = s.write_all(&b); }
    }
}

fn broadcast_targets() -> Vec<Ipv4Addr> {
    let mut v = vec![Ipv4Addr::BROADCAST];
    if let Ok(ifs) = if_addrs::get_if_addrs() {
        for i in ifs {
            if let if_addrs::IfAddr::V4(a) = i.addr {
                if !a.ip.is_loopback() {
                    v.push(a.broadcast.unwrap_or_else(|| { let o = a.ip.octets(); Ipv4Addr::new(o[0], o[1], o[2], 255) }));
                }
            }
        }
    }
    v.sort();
    v.dedup();
    v
}

fn discovery_responder(line: String) {
    let sock = match Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP)) { Ok(s) => s, Err(_) => return };
    let _ = sock.set_reuse_address(true);
    #[cfg(all(unix, not(target_os = "solaris"), not(target_os = "illumos")))]
    let _ = sock.set_reuse_port(true);                                  // macOS shares a UDP port only with this
    if sock.bind(&SocketAddr::from((Ipv4Addr::UNSPECIFIED, DISCOVERY_PORT)).into()).is_err() {
        eprintln!("note: UDP {} is busy; devices will find this by its announcements only", DISCOVERY_PORT);
        return;
    }
    let udp: UdpSocket = sock.into();
    let mut b = [0u8; 64];
    loop {
        if let Ok((n, from)) = udp.recv_from(&mut b) {
            if b[..n].trim_ascii() == b"PURR-REPO?" {
                let _ = udp.send_to(line.as_bytes(), from);
            }
        }
    }
}

fn announcer(line: String) {
    let Ok(sock) = UdpSocket::bind("0.0.0.0:0") else { return };
    let _ = sock.set_broadcast(true);
    loop {
        for t in broadcast_targets() {
            let _ = sock.send_to(line.as_bytes(), (t, DISCOVERY_PORT));
        }
        thread::sleep(Duration::from_secs(3));
    }
}

/// Blocks serving until the process is stopped.
pub fn run(files: BTreeMap<String, Vec<u8>>, name: &str, port: u16, bind: &str) -> Result<(), String> {
    let manifest = files.get("recovery.manifest").cloned().unwrap_or_default();
    let line = announce_line(name, port, &relay_id(&manifest));
    let listener = TcpListener::bind((bind, port)).map_err(|e| format!("cannot listen on {}:{}: {}", bind, port, e))?;
    thread::spawn({ let l = line.clone(); move || discovery_responder(l) });
    thread::spawn({ let l = line.clone(); move || announcer(l) });
    println!("serving {} file(s) as \"{}\" on port {}; announcing: {}", files.len(), name, port, line);
    println!("a device in the VM reaches this at http://10.0.2.2:{}; local network only. Ctrl-C to stop.", port);
    let files = Arc::new(files);
    let log: Arc<dyn Fn(String) + Send + Sync> = Arc::new(|m| println!("{}", m));
    for conn in listener.incoming().flatten() {
        let (f, l) = (files.clone(), log.clone());
        thread::spawn(move || handle(conn, &f, &*l));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn files() -> BTreeMap<String, Vec<u8>> {
        let mut m = BTreeMap::new();
        m.insert("recovery.manifest".to_string(), b"release=1\n".to_vec());
        m.insert("kernel_tdeck_plus.cat".to_string(), vec![1, 2, 3]);
        m
    }

    #[test]
    fn serves_only_what_was_checked() {
        let f = files();
        assert_eq!(respond("GET /recovery/kernel_tdeck_plus.cat HTTP/1.1", &f), (200, Some(vec![1, 2, 3])));
        assert_eq!(respond("GET /recovery/recovery.manifest?x=1 HTTP/1.1", &f).0, 200);
        assert_eq!(respond("GET /recovery/../Cargo.toml HTTP/1.1", &f).0, 404);
        assert_eq!(respond("GET /etc/passwd HTTP/1.1", &f).0, 404);
        assert_eq!(respond("POST /recovery/recovery.manifest HTTP/1.1", &f).0, 405);
    }

    #[test]
    fn announcement_matches_what_the_device_parses() {
        let line = announce_line("Claude's Mac", 47801, &relay_id(b"m"));
        assert!(line.starts_with("PURR-REPO! Claude's_Mac 47801 relay-"));
        assert_eq!(line.split(' ').count(), 4);
    }
}
