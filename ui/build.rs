fn main() {
    // The icon is resource id 1: GPUI's Windows backend loads id 1 from the
    // executable module for its window class.
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        embed_resource::compile("assets/app.rc", embed_resource::NONE)
            .manifest_optional()
            .unwrap();
    }
}
