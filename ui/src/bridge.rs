//! Bridge to the C++ stream engine.
//!
//! The engine (`brpnext-engine.exe --engine --bridge-port N`) owns streaming,
//! decode, audio, input, DLSS and the in-stream overlay. The shell owns the
//! window and the settings UI. Commands are plain lines over a loopback TCP
//! socket; status snapshots come back as `S <json>`.
//!
//! The port is chosen by the shell and passed on the command line, so the
//! bridge never listens on a well-known port and never triggers a firewall
//! prompt: loopback traffic is not filtered.

use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{self, Sender};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use serde_json::Value;

use crate::model::Status;

#[derive(Default)]
pub struct Shared {
    pub status: Status,
    pub rev: u64,
    pub online: bool,
}

pub struct Engine {
    child: Child,
    tx: Sender<String>,
    pub shared: Arc<Mutex<Shared>>,
}

impl Engine {
    pub fn start(path: &Path) -> Option<Self> {
        // Reserve a free loopback port, then hand it to the engine.
        let listener = TcpListener::bind("127.0.0.1:0").ok()?;
        let port = listener.local_addr().ok()?.port();
        drop(listener);

        let mut child = Command::new(path)
            .arg("--engine")
            .arg("--bridge-port")
            .arg(port.to_string())
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .ok()?;
        let (tx, rx) = mpsc::channel::<String>();
        let shared = Arc::new(Mutex::new(Shared::default()));
        let state = Arc::clone(&shared);

        thread::spawn(move || {
            crate::model::log_line(&format!("bridge: connecting to 127.0.0.1:{port}"));
            let mut pipe: Option<TcpStream> = None;
            for _ in 0..200 {
                match TcpStream::connect(("127.0.0.1", port)) {
                    Ok(stream) => {
                        pipe = Some(stream);
                        break;
                    }
                    Err(_) => thread::sleep(Duration::from_millis(50)),
                }
            }
            let Some(stream) = pipe else {
                crate::model::log_line("bridge: connect failed");
                return;
            };
            let _ = stream.set_nodelay(true);
            let Ok(reader_stream) = stream.try_clone() else {
                crate::model::log_line("bridge: clone failed");
                return;
            };
            crate::model::log_line("bridge: connected");
            state.lock().unwrap().online = true;

            thread::spawn(move || {
                let mut stream = stream;
                for command in rx {
                    crate::model::log_line(&format!("bridge: send {command}"));
                    if writeln!(stream, "{command}").is_err() {
                        crate::model::log_line("bridge: write failed");
                        break;
                    }
                    let _ = stream.flush();
                }
            });

            let mut reader = BufReader::new(reader_stream);
            let mut line = String::new();
            loop {
                line.clear();
                match reader.read_line(&mut line) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {}
                }
                let Some(body) = line.strip_prefix("S ") else {
                    continue;
                };
                let Ok(value) = serde_json::from_str::<Value>(body.trim()) else {
                    continue;
                };
                let status = Status::from_json(&value);
                let mut state = state.lock().unwrap();
                state.status = status;
                state.rev += 1;
                if state.rev <= 3 {
                    let rev = state.rev;
                    let name = state.status.state.clone();
                    drop(state);
                    crate::model::log_line(&format!("bridge: status rev {rev} state {name}"));
                }
            }
            crate::model::log_line("bridge: reader ended");
            state.lock().unwrap().online = false;
        });

        if let Ok(Some(_)) = child.try_wait() {
            return None;
        }

        Some(Self { child, tx, shared })
    }

    pub fn send(&self, command: &str) {
        let _ = self.tx.send(command.to_string());
    }

    pub fn online(&self) -> bool {
        self.shared.lock().map(|s| s.online).unwrap_or(false)
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        self.send("shutdown");
        for _ in 0..12 {
            if let Ok(Some(_)) = self.child.try_wait() {
                return;
            }
            thread::sleep(Duration::from_millis(25));
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Locates the engine next to the shell, or in the development build tree.
pub fn engine_path() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("BRP_ENGINE") {
        let path = PathBuf::from(path);
        if path.exists() {
            return Some(path);
        }
    }
    let exe = std::env::current_exe().ok()?;
    let dir = exe.parent()?.to_path_buf();
    let mut candidates = vec![dir.join("brpnext-engine.exe")];
    // ui/target/debug -> repository root
    candidates.push(dir.join(r"..\..\..\build-perf\Release\brpnext-engine.exe"));
    candidates.push(dir.join(r"..\..\..\build\Release\brpnext-engine.exe"));
    candidates.into_iter().find(|path| path.exists())
}
