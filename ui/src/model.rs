//! Settings model.
//!
//! The engine stores a flat JSON object with dotted keys. We keep that exact
//! shape (no per-field structs) so unknown keys written by the engine survive a
//! round trip and no setting can silently be dropped by the new shell.

use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::PathBuf;

use serde_json::{Map, Value};

pub fn config_dir() -> PathBuf {
    let base = std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("BetterRemotePlayNext")
}

pub fn settings_path() -> PathBuf {
    config_dir().join("next-settings.json")
}

pub fn log_path() -> PathBuf {
    config_dir().join("ui.log")
}

#[derive(Clone, Debug)]
pub struct Settings {
    path: PathBuf,
    values: Map<String, Value>,
    pub dirty: bool,
}

impl Settings {
    pub fn load() -> Self {
        let path = settings_path();
        let mut values = Map::new();
        match fs::read_to_string(&path) {
            Ok(text) => match serde_json::from_str::<Value>(&text) {
                Ok(Value::Object(map)) => values = map,
                Ok(_) => {
                    log_line("settings: root is not an object");
                    Self::quarantine(&path);
                }
                Err(error) => {
                    log_line(&format!("settings: parse failed: {error}"));
                    Self::quarantine(&path);
                }
            },
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => {
                log_line(&format!("settings: read failed: {error}"));
            }
        }
        let mut settings = Self {
            path,
            values,
            dirty: false,
        };
        settings.ensure_defaults();
        settings
    }

    /// Moves a file we could not parse out of the way instead of letting the
    /// next save destroy it.
    fn quarantine(path: &PathBuf) {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let broken = path.with_extension(format!("json.corrupt-{stamp}"));
        if let Err(error) = fs::rename(path, &broken) {
            log_line(&format!("settings: quarantine failed: {error}"));
        }
    }

    /// Defaults mirror the engine's own defaults so a fresh install behaves
    /// exactly like the C++ shell.
    pub fn ensure_defaults(&mut self) {
        let defaults: &[(&str, Value)] = &[
            ("schema", Value::from(1)),
            ("general.theme", Value::from("dark")),
            ("general.start_minimized", Value::from(false)),
            ("general.close_to_tray", Value::from(false)),
            ("general.notifications", Value::from(true)),
            ("general.check_updates", Value::from(true)),
            ("general.update_channel", Value::from(0)),
            ("general.telemetry", Value::from(false)),
            ("console.name", Value::from("PS5")),
            ("console.address", Value::from("")),
            ("console.auto_discover", Value::from(true)),
            ("console.auto_reconnect", Value::from(true)),
            ("console.reconnect_delay_ms", Value::from(1500)),
            ("streaming.input", Value::from("1080p")),
            ("streaming.fps", Value::from(60)),
            ("streaming.codec", Value::from("auto")),
            ("streaming.decoder", Value::from("auto")),
            ("streaming.bitrate_kbps", Value::from(50000)),
            ("streaming.frame_buffer", Value::from(1)),
            ("streaming.hdr", Value::from(false)),
            ("video.output", Value::from("native")),
            ("video.vsync", Value::from(true)),
            ("video.vrr", Value::from(true)),
            ("video.hdr", Value::from(false)),
            ("video.color_space", Value::from("auto")),
            ("video.sharpening", Value::from(10)),
            ("video.render_target", Value::from(0)),
            ("ai.super_resolution", Value::from(false)),
            ("ai.sr_output", Value::from("native")),
            ("ai.sr_model", Value::from("dlss")),
            ("ai.sr_quality", Value::from(4)),
            ("ai.temporal", Value::from(false)),
            ("ai.frame_generation", Value::from(false)),
            ("ai.fg_backend", Value::from("none")),
            ("ai.fg_multiplier", Value::from(2)),
            ("ai.quality", Value::from(1)),
            ("ai.occlusion", Value::from(true)),
            ("ai.adaptive", Value::from(true)),
            ("ai.rife_model", Value::from(0)),
            ("ai.blend", Value::from(10)),
            ("ai.motion_safety", Value::from(true)),
            ("ai.neural_render", Value::from(false)),
            ("ai.neural_style", Value::from(2)),
            ("ai.neural_intensity", Value::from(100)),
            ("ai.neural_passes", Value::from(1)),
            ("ai.neural_scale", Value::from(100)),
            ("ai.neural_ui", Value::from(false)),
            ("ai.neural_mask", Value::from(false)),
            ("gpu.device", Value::from(0)),
            ("gpu.fp16", Value::from(true)),
            ("gpu.int8", Value::from(false)),
            ("gpu.frame_buffers", Value::from(2)),
            ("gpu.memory_limit_mb", Value::from(0)),
            ("gpu.priority", Value::from(0)),
            ("display.monitor", Value::from(0)),
            ("display.refresh", Value::from("match")),
            ("display.vsync", Value::from(true)),
            ("display.vrr", Value::from(true)),
            ("display.fullscreen_on_play", Value::from(true)),
            ("display.output_format", Value::from("auto")),
            ("display.bit_depth", Value::from(8)),
            ("overlay.enabled", Value::from(false)),
            ("overlay.compact", Value::from(false)),
            ("overlay.opacity", Value::from(0.85)),
            ("overlay.position", Value::from(1)),
            ("overlay.side", Value::from(1)),
            ("overlay.scale", Value::from(125)),
            ("overlay.interval_ms", Value::from(250)),
            ("overlay.show_performance", Value::from(true)),
            ("overlay.show_gpu", Value::from(true)),
            ("overlay.show_stream", Value::from(true)),
            ("overlay.show_processing", Value::from(true)),
            ("overlay.hotkey", Value::from("F10")),
            ("audio.device", Value::from("default")),
            ("audio.buffer_ms", Value::from(20)),
            ("audio.volume", Value::from(100)),
            ("audio.mute", Value::from(false)),
            ("controller.enabled", Value::from(true)),
            ("controller.haptics", Value::from(true)),
            ("controller.adaptive_triggers", Value::from(true)),
            ("controller.rumble_strength", Value::from(5)),
            ("controller.poll_hz", Value::from(250)),
            ("controller.lightbar", Value::from(0)),
            ("advanced.experimental", Value::from(false)),
            ("advanced.debug_log", Value::from(false)),
            ("advanced.log_level", Value::from("info")),
            ("advanced.prefer_gpu_present", Value::from(true)),
            ("ui.scale", Value::from(100)),
            ("ui.animated_background", Value::from(true)),
            ("ui.boot_animation", Value::from(true)),
            ("ui.reduce_motion", Value::from(false)),
        ];
        for (key, value) in defaults {
            self.values.entry((*key).to_string()).or_insert_with(|| value.clone());
        }
    }

    pub fn b(&self, key: &str, fallback: bool) -> bool {
        self.values
            .get(key)
            .and_then(Value::as_bool)
            .unwrap_or(fallback)
    }

    pub fn i(&self, key: &str, fallback: i64) -> i64 {
        self.values
            .get(key)
            .and_then(Value::as_i64)
            .unwrap_or(fallback)
    }

    pub fn f(&self, key: &str, fallback: f64) -> f64 {
        self.values
            .get(key)
            .and_then(Value::as_f64)
            .unwrap_or(fallback)
    }

    pub fn s(&self, key: &str, fallback: &str) -> String {
        self.values
            .get(key)
            .and_then(Value::as_str)
            .unwrap_or(fallback)
            .to_string()
    }

    pub fn set(&mut self, key: &str, value: Value) {
        if self.values.get(key) != Some(&value) {
            self.values.insert(key.to_string(), value);
            self.dirty = true;
        }
    }

    pub fn set_bool(&mut self, key: &str, value: bool) {
        self.set(key, Value::from(value));
    }

    pub fn set_int(&mut self, key: &str, value: i64) {
        self.set(key, Value::from(value));
    }

    pub fn set_float(&mut self, key: &str, value: f64) {
        self.set(key, Value::from(value));
    }

    pub fn set_str(&mut self, key: &str, value: &str) {
        self.set(key, Value::from(value));
    }

    pub fn flip(&mut self, key: &str) {
        let next = !self.b(key, false);
        self.set_bool(key, next);
    }

    pub fn bump(&mut self, key: &str, delta: i64, min: i64, max: i64, fallback: i64) {
        let next = (self.i(key, fallback) + delta).clamp(min, max);
        self.set_int(key, next);
    }

    pub fn bump_f(&mut self, key: &str, delta: f64, min: f64, max: f64, fallback: f64) {
        let next = (self.f(key, fallback) + delta).clamp(min, max);
        self.set_float(key, (next * 100.0).round() / 100.0);
    }

    /// Saves through a temporary file and keeps one backup of the previous
    /// file. Both the engine and the shell read this path, so a half-written
    /// file would break the next launch.
    pub fn save(&mut self) -> io::Result<()> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent)?;
        }
        let body = serde_json::to_string_pretty(&Value::Object(self.values.clone()))
            .map_err(io::Error::other)?;
        let tmp = self.path.with_extension("json.tmp");
        fs::write(&tmp, body.as_bytes())?;
        if self.path.exists() {
            let backup = self.path.with_extension("json.bak");
            let _ = fs::copy(&self.path, backup);
        }
        // Windows cannot rename over an existing file.
        let _ = fs::remove_file(&self.path);
        fs::rename(&tmp, &self.path)?;
        self.dirty = false;
        Ok(())
    }

    pub fn reload(&mut self) {
        *self = Self::load();
    }

    /// Modification time of the settings file, for detecting edits made by the
    /// overlay window or by the engine.
    pub fn file_mtime(&self) -> Option<std::time::SystemTime> {
        fs::metadata(&self.path).and_then(|meta| meta.modified()).ok()
    }

    pub fn reset(&mut self) {
        self.values.clear();
        self.ensure_defaults();
        self.dirty = true;
    }
    /// A named bundle of settings. Each preset writes real values only.
    pub fn apply_preset(&mut self, preset: Preset) {
        match preset {
            Preset::Balanced => {
                self.set_int("streaming.fps", 60);
                self.set_int("streaming.frame_buffer", 1);
                self.set_int("streaming.bitrate_kbps", 60000);
                self.set_bool("ai.super_resolution", true);
                self.set_int("ai.sr_quality", 3);
                self.set_bool("ai.frame_generation", false);
                self.set_int("controller.poll_hz", 500);
            }
            Preset::Performance => {
                self.set_int("streaming.fps", 60);
                self.set_int("streaming.frame_buffer", 0);
                self.set_int("streaming.bitrate_kbps", 45000);
                self.set_bool("ai.super_resolution", false);
                self.set_bool("ai.frame_generation", true);
                self.set_int("ai.fg_multiplier", 2);
                self.set_bool("ai.occlusion", true);
                self.set_int("controller.poll_hz", 1000);
            }
            Preset::Cinematic => {
                self.set_int("streaming.fps", 30);
                self.set_int("streaming.frame_buffer", 1);
                self.set_int("streaming.bitrate_kbps", 100000);
                self.set_bool("ai.super_resolution", true);
                self.set_int("ai.sr_quality", 5);
                self.set_bool("ai.frame_generation", true);
                self.set_int("ai.fg_multiplier", 2);
                self.set_int("controller.poll_hz", 1000);
            }
            Preset::LowLatency => {
                self.set_int("streaming.fps", 60);
                self.set_int("streaming.frame_buffer", 0);
                self.set_int("streaming.bitrate_kbps", 50000);
                self.set_bool("ai.super_resolution", false);
                self.set_bool("ai.frame_generation", false);
                self.set_int("controller.poll_hz", 1000);
            }
        }
        self.dirty = true;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Preset {
    LowLatency,
    Performance,
    Balanced,
    Cinematic,
}

impl Preset {
    pub fn label(self) -> &'static str {
        match self {
            Preset::LowLatency => "Low latency",
            Preset::Performance => "Performance",
            Preset::Balanced => "Balanced",
            Preset::Cinematic => "Cinematic",
        }
    }

    pub fn hint(self) -> &'static str {
        match self {
            Preset::LowLatency => "Buffer 0 · no AI · 60 fps",
            Preset::Performance => "FG on · buffer 0 · 1000 Hz",
            Preset::Balanced => "SR quality · buffer 1",
            Preset::Cinematic => "30 fps · DLAA · FG 2x",
        }
    }
}

/// Status pushed by the engine over the bridge.
#[derive(Clone, Debug, Default)]
pub struct Status {
    pub state: String,
    pub connected: bool,
    pub streaming: bool,
    pub connecting: bool,
    pub armed: bool,
    pub message: String,
    pub console_name: String,
    pub codec: String,
    pub width: i64,
    pub height: i64,
    pub fps: f64,
    pub bitrate_kbps: f64,
    pub loss_percent: f64,
    pub jitter_ms: f64,
    pub decode_ms: f64,
    pub decode_max: f64,
    pub loop_p99: f64,
    pub drops: i64,
    pub cadence_p50: f64,
    pub cadence_p99: f64,
    pub scanout_hz: f64,
    pub present_misses: f64,
    pub shown_fps: f64,
    pub restart_pending: bool,
    pub sr_ms: f64,
    pub fg_ms: f64,
    pub generated: i64,
    pub cuda: bool,
    pub tensorrt: bool,
    pub dlss_fg_ready: bool,
    pub dlss_sr_ready: bool,
    pub nr_runtime: bool,
    pub nr_ready: bool,
    pub nr_ms: f64,
    pub gpu: String,
    pub driver: String,
    pub vram_mb: i64,
}

impl Status {
    pub fn from_json(value: &Value) -> Self {
        let g = |k: &str| value.get(k);
        let s = |k: &str| g(k).and_then(Value::as_str).unwrap_or_default().to_string();
        let i = |k: &str| g(k).and_then(Value::as_i64).unwrap_or(0);
        let f = |k: &str| g(k).and_then(Value::as_f64).unwrap_or(0.0);
        let b = |k: &str| g(k).and_then(Value::as_bool).unwrap_or(false);
        Self {
            state: s("state"),
            connected: b("connected"),
            streaming: b("streaming"),
            connecting: b("connecting"),
            armed: b("armed"),
            message: s("message"),
            console_name: s("console_name"),
            codec: s("codec"),
            width: i("width"),
            height: i("height"),
            fps: f("fps"),
            bitrate_kbps: f("bitrate"),
            loss_percent: f("loss"),
            jitter_ms: f("jitter"),
            decode_ms: f("decode_ms"),
            decode_max: f("decode_max"),
            loop_p99: f("loop_p99"),
            drops: i("drops"),
            cadence_p50: f("cadence_p50"),
            cadence_p99: f("cadence_p99"),
            scanout_hz: f("scanout_hz"),
            present_misses: f("present_misses"),
            shown_fps: f("shown_fps"),
            restart_pending: b("restart_pending"),
            sr_ms: f("sr_ms"),
            fg_ms: f("fg_ms"),
            generated: i("generated"),
            cuda: b("cuda"),
            tensorrt: b("tensorrt"),
            dlss_fg_ready: b("dlss_fg_ready"),
            dlss_sr_ready: b("dlss_sr_ready"),
            nr_runtime: b("nr_runtime"),
            nr_ready: b("nr_ready"),
            nr_ms: f("nr_ms"),
            gpu: s("gpu"),
            driver: s("driver"),
            vram_mb: i("vram_mb"),
        }
    }

    pub fn is_live(&self) -> bool {
        self.streaming
    }

    pub fn headline(&self) -> &'static str {
        if self.connecting {
            "Connecting"
        } else if self.streaming {
            "Streaming"
        } else if self.connected {
            "Connected"
        } else {
            "Idle"
        }
    }
}

pub type MapAlias = BTreeMap<String, Value>;

/// Small append-only log for shell-side diagnostics. The engine keeps its own
/// log next to this file.
pub fn log_line(message: &str) {
    use std::io::Write;
    static LOG_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let _guard = LOG_LOCK.lock();
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    if let Ok(mut file) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(log_path())
    {
        let _ = writeln!(file, "{stamp} {message}");
    }
}
