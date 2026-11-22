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

#[derive(Clone, Debug, Default)]
pub struct Device {
    pub id: String,
    pub name: String,
    pub is_default: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Monitor {
    pub index: i64,
    pub name: String,
    pub width: i64,
    pub height: i64,
    pub hz: i64,
    pub primary: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Devices {
    pub audio: Vec<Device>,
    pub monitors: Vec<Monitor>,
    pub gpus: Vec<String>,
}

impl Devices {
    fn from_json(value: &Value) -> Self {
        let mut devices = Devices::default();
        if let Some(list) = value.get("audio").and_then(Value::as_array) {
            for item in list {
                devices.audio.push(Device {
                    id: item.get("id").and_then(Value::as_str).unwrap_or_default().to_string(),
                    name: item.get("name").and_then(Value::as_str).unwrap_or_default().to_string(),
                    is_default: item.get("default").and_then(Value::as_bool).unwrap_or(false),
                });
            }
        }
        if let Some(list) = value.get("monitors").and_then(Value::as_array) {
            for item in list {
                devices.monitors.push(Monitor {
                    index: item.get("index").and_then(Value::as_i64).unwrap_or(0),
                    name: item.get("name").and_then(Value::as_str).unwrap_or_default().to_string(),
                    width: item.get("width").and_then(Value::as_i64).unwrap_or(0),
                    height: item.get("height").and_then(Value::as_i64).unwrap_or(0),
                    hz: item.get("hz").and_then(Value::as_i64).unwrap_or(0),
                    primary: item.get("primary").and_then(Value::as_bool).unwrap_or(false),
                });
            }
        }
        if let Some(list) = value.get("gpus").and_then(Value::as_array) {
            for item in list {
                if let Some(name) = item.as_str() {
                    devices.gpus.push(name.to_string());
                }
            }
        }
        devices
    }
}

#[derive(Default)]
pub struct Shared {
    pub status: Status,
    pub rev: u64,
    pub online: bool,
    pub devices: Option<Devices>,
    pub devices_rev: u64,
    pub events: Vec<String>,
}

struct EngineInner {
    child: Mutex<Child>,
    tx: Sender<String>,
}

#[derive(Clone)]
pub struct Engine {
    inner: Arc<EngineInner>,
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
                if let Some(body) = line.strip_prefix("E ") {
                    let body = body.trim();
                    if body == "overlay-toggle" {
                        crate::model::log_line("bridge: event overlay-toggle");
                        let mut state = state.lock().unwrap();
                        state.events.push("overlay-toggle".to_string());
                    } else if let Ok(value) = serde_json::from_str::<Value>(body) {
                        match value.get("kind").and_then(Value::as_str) {
                            Some("devices") => {
                            let devices = Devices::from_json(&value);
                            let mut state = state.lock().unwrap();
                            state.devices = Some(devices);
                            state.devices_rev += 1;
                            if state.devices_rev <= 2 {
                                let a = state.devices.as_ref().map(|d| d.audio.len()).unwrap_or(0);
                                let m = state.devices.as_ref().map(|d| d.monitors.len()).unwrap_or(0);
                                let g = state.devices.as_ref().map(|d| d.gpus.len()).unwrap_or(0);
                                drop(state);
                                crate::model::log_line(&format!(
                                    "bridge: devices audio={a} monitors={m} gpus={g}"
                                ));
                            }
                            }
                            _ => {}
                        }
                    }
                    continue;
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

        Some(Self {
            inner: Arc::new(EngineInner {
                child: Mutex::new(child),
                tx,
            }),
            shared,
        })
    }

    pub fn send(&self, command: &str) {
        let _ = self.inner.tx.send(command.to_string());
    }

    pub fn online(&self) -> bool {
        self.shared.lock().map(|s| s.online).unwrap_or(false)
    }
}

impl Drop for EngineInner {
    fn drop(&mut self) {
        let _ = self.tx.send("shutdown".to_string());
        let Ok(mut child) = self.child.lock() else {
            return;
        };
        for _ in 0..12 {
            if let Ok(Some(_)) = child.try_wait() {
                return;
            }
            thread::sleep(Duration::from_millis(25));
        }
        let _ = child.kill();
        let _ = child.wait();
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
