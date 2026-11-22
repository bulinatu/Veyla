mod app;
mod bridge;
mod background;
mod controls;
mod model;
mod pages;
mod theme;
mod widgets;

use std::borrow::Cow;

use gpui::{
    prelude::*, px, size, App, Application, Bounds, TitlebarOptions, WindowBounds, WindowOptions,
};

fn main() {
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
