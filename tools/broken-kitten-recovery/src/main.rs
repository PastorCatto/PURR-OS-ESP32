//! broken-kitten-recovery: downloads the official PURR OS recovery images, checks every one against the official keys, and serves
//! them to a device on the local network that cannot reach the internet or whose recovery is broken. The device checks them again
//! itself: this tool's word is never what it relies on.

mod image;
mod keys;
mod library;
mod manifest;
mod serve;

use std::path::PathBuf;
use std::process::ExitCode;

const DEFAULT_RELEASE: &str = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar1";
const DEFAULT_MODULES: &str = "https://github.com/PastorCatto/PURR-OS-ESP32/releases/download/JumpingJaguar2";

const HELP: &str = "broken-kitten-recovery: restore a PURR OS device from this computer, no USB cable.

USAGE: broken-kitten-recovery [COMMAND] [OPTIONS]

COMMANDS
  run      fetch, check, then serve (the default)
  fetch    download and check the images into the folder
  verify   check what is already in the folder, change nothing
  serve    check what is in the folder, then serve it
  add-driver  check a driver file (--file F --compatible C --buses B --catcall K) and add it to drivers.manifest

OPTIONS
  --dir DIR           where the images are kept            (default: ./kitten-recovery)
  --board NAME        the device's board                   (default: tdeck_plus)
  --release-url URL   where recovery.manifest is           (default: the official release)
  --modules-url URL   where modules.manifest is            (default: the official modules release)
  --name NAME         how the device sees this computer    (default: the computer's name)
  --port N            HTTP port                            (default: 47801)
  --bind ADDR         address to listen on                 (default: 0.0.0.0)
  --dev               developer role: also serve the drivers added with add-driver, signed or not (a device only loads
                      an unsigned one in developer mode)
  --file PATH         the driver file for add-driver
  --compatible STR    what the driver serves, e.g. st7789       --buses STR (spi, i2c)       --catcall STR (display, touch, ...)
  --offline           with run: skip the download, serve the folder as it is
  -h, --help          this text

Every image is checked against the official keys (hash, signature, who may sign what) when it is downloaded and again just before
it is served. Nothing that fails is served. Only a device on the same local network will use it.";

struct Opts {
    command: String,
    dir: PathBuf,
    board: String,
    release: String,
    modules: String,
    name: String,
    port: u16,
    bind: String,
    offline: bool,
    dev: bool,
    file: Option<PathBuf>,
    compatible: String,
    buses: String,
    catcall: String,
}

fn computer_name() -> String {
    for k in ["COMPUTERNAME", "HOSTNAME", "HOST", "USER", "USERNAME"] {
        if let Ok(v) = std::env::var(k) {
            if !v.is_empty() { return v; }
        }
    }
    "computer".to_string()
}

fn parse_args(args: &[String]) -> Result<Opts, String> {
    let mut o = Opts { command: "run".into(), dir: "kitten-recovery".into(), board: "tdeck_plus".into(), release: DEFAULT_RELEASE.into(),
                       modules: DEFAULT_MODULES.into(), name: format!("{}_relay", computer_name()), port: 47801, bind: "0.0.0.0".into(), offline: false,
                       dev: false, file: None, compatible: String::new(), buses: String::new(), catcall: String::new() };
    let mut i = 0;
    let mut first = true;
    while i < args.len() {
        let a = args[i].as_str();
        let mut val = || -> Result<String, String> { i += 1; args.get(i).cloned().ok_or(format!("{} needs a value", a)) };
        match a {
            "run" | "fetch" | "verify" | "serve" | "add-driver" if first => o.command = a.to_string(),
            "--dir" => o.dir = val()?.into(),
            "--board" => o.board = val()?,
            "--release-url" => o.release = val()?,
            "--modules-url" => o.modules = val()?,
            "--name" => o.name = val()?,
            "--port" => o.port = val()?.parse().map_err(|_| "--port needs a number")?,
            "--bind" => o.bind = val()?,
            "--offline" => o.offline = true,
            "--dev" => o.dev = true,
            "--file" => o.file = Some(val()?.into()),
            "--compatible" => o.compatible = val()?,
            "--buses" => o.buses = val()?,
            "--catcall" => o.catcall = val()?,
            "-h" | "--help" => return Err(String::new()),
            _ => return Err(format!("unknown option \"{}\"", a)),
        }
        first = false;
        i += 1;
    }
    Ok(o)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let o = match parse_args(&args) {
        Ok(o) => o,
        Err(e) => {
            if e.is_empty() { println!("{}", HELP); return ExitCode::SUCCESS; }
            eprintln!("error: {}\n\n{}", e, HELP);
            return ExitCode::from(2);
        }
    };
    let keys = library::official_keys();
    let mut log = |m: &str| println!("{}", m);
    if o.command == "add-driver" {
        let Some(file) = &o.file else { eprintln!("add-driver needs --file"); return ExitCode::from(2) };
        if o.compatible.is_empty() || o.buses.is_empty() || o.catcall.is_empty() {
            eprintln!("add-driver needs --compatible, --buses and --catcall");
            return ExitCode::from(2);
        }
        return match library::add_driver(&o.dir, file, &o.board, &o.compatible, &o.buses, &o.catcall, keys) {
            Ok(m) => { println!("{}", m); ExitCode::SUCCESS }
            Err(e) => { eprintln!("REFUSED: {}", e); ExitCode::from(1) }
        };
    }
    let download = matches!(o.command.as_str(), "run" | "fetch") && !o.offline;
    if download {
        let r = library::fetch(&o.release, "recovery.manifest", &o.dir, &o.board, keys, &mut log)
            .and_then(|a| library::fetch(&o.modules, "modules.manifest", &o.dir, &o.board, keys, &mut log).map(|b| a + b));
        match r {
            Ok(n) => { library::fetch_legal(&o.release, &o.dir); println!("downloaded and checked {} image(s)", n); }
            Err(e) => { eprintln!("REFUSED: {}", e); eprintln!("nothing was served"); return ExitCode::from(1); }
        }
    }
    if o.command == "fetch" { return ExitCode::SUCCESS; }
    println!("checking {} against the official keys...", o.dir.display());
    let files = match library::check_dir(&o.dir, &o.board, keys, o.dev, &mut log) {
        Ok(f) => f,
        Err(e) => { eprintln!("REFUSED: {}", e); eprintln!("nothing was served"); return ExitCode::from(1); }
    };
    if o.command == "verify" { println!("all good"); return ExitCode::SUCCESS; }
    match serve::run(files, &o.name, o.port, &o.bind) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => { eprintln!("error: {}", e); ExitCode::from(1) }
    }
}
