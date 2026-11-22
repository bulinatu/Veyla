mod app;
mod bridge;
mod background;
mod controls;
mod model;
mod overlay;
mod pages;
mod theme;
mod widgets;

use std::borrow::Cow;

use gpui::{
    prelude::*, px, size, App, Application, Bounds, TitlebarOptions, WindowBounds, WindowOptions,
};

#[link(name = "user32")]
extern "system" {
    fn FindWindowW(class: *const u16, title: *const u16) -> isize;
    fn SetForegroundWindow(hwnd: isize) -> i32;
}

#[link(name = "kernel32")]
extern "system" {
    fn CreateMutexW(attributes: *const core::ffi::c_void, owner: i32, name: *const u16) -> isize;
    fn GetLastError() -> u32;
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

/// Returns false when another shell already owns the singleton mutex; the
/// existing window is brought forward instead of starting a second engine.
fn claim_single_instance() -> bool {
    let name = wide("BetterRemotePlayNextShell");
    unsafe {
        let handle = CreateMutexW(core::ptr::null(), 0, name.as_ptr());
        if handle == 0 {
            return true;
        }
        if GetLastError() == 183 {
            // ERROR_ALREADY_EXISTS
            let title = wide("BetterRemotePlay");
            let hwnd = FindWindowW(core::ptr::null(), title.as_ptr());
            if hwnd != 0 {
                SetForegroundWindow(hwnd);
            }
            return false;
        }
        // Intentionally leak the handle: it must live as long as the process.
    }
    true
}

fn main() {
    std::panic::set_hook(Box::new(|info| {
        app::log_panic(&format!("{info}"));
    }));
    if !claim_single_instance() {
        return;
    }
    Application::new().run(|cx: &mut App| {
        let fonts: Vec<Cow<'static, [u8]>> = vec![
            Cow::Borrowed(include_bytes!("../assets/fonts/Inter-Regular.ttf")),
            Cow::Borrowed(include_bytes!("../assets/fonts/Inter-Medium.ttf")),
            Cow::Borrowed(include_bytes!("../assets/fonts/Inter-SemiBold.ttf")),
            Cow::Borrowed(include_bytes!("../assets/fonts/Inter-Bold.ttf")),
            Cow::Borrowed(include_bytes!("../assets/fonts/JetBrainsMono-Regular.ttf")),
            Cow::Borrowed(include_bytes!("../assets/fonts/JetBrainsMono-Medium.ttf")),
            Cow::Borrowed(include_bytes!("../assets/fonts/JetBrainsMono-Bold.ttf")),
        ];
        if let Err(error) = cx.text_system().add_fonts(fonts) {
            eprintln!("font load failed: {error}");
        }

        let bounds = Bounds::centered(None, size(px(1340.0), px(880.0)), cx);
        let options = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(bounds)),
            titlebar: Some(TitlebarOptions {
                title: Some("BetterRemotePlay".into()),
                appears_transparent: true,
                traffic_light_position: None,
            }),
            window_min_size: Some(size(px(1040.0), px(700.0))),
            app_id: Some("BetterRemotePlayNext".to_string()),
            focus: true,
            show: true,
            ..Default::default()
        };

        cx.open_window(options, |_window, cx| cx.new(app::Root::new))
            .expect("open main window");
        cx.activate(true);
    });
}
