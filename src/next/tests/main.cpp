// Deterministic foundation tests. No framework: exit code 0 on success.
// Build target: brp_next_tests.

#include "app/app_host.h"
#include "app/navigation.h"
#include "config/flat_json.h"
#include "config/profiles.h"
#include "config/settings.h"
#include "core/models.h"
#include "diagnostics/metrics.h"
#include "ui/theme.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

bool saved_id_valid(const std::string& id);

void Expect(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "  ok   " << message << "\n";
    } else {
        std::cout << "  FAIL " << message << "\n";
        ++g_failures;
    }
}

bool Near(double a, double b, double tolerance = 1e-6) {
    return std::abs(a - b) <= tolerance;
}

std::filesystem::path TempRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("brp-next-tests-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    return root;
}

void TestFlatJson() {
    std::cout << "FlatJson\n";
    brp::next::FlatMap map;
    brp::next::FlatJson::PutString(map, "name", "PS5 \"Living Room\"\\den");
    brp::next::FlatJson::PutInt(map, "fps", 120);
    brp::next::FlatJson::PutBool(map, "hdr", true);
    brp::next::FlatJson::PutDouble(map, "opacity", 0.75);

    const std::string text = brp::next::FlatJson::Serialize(map);
    brp::next::FlatMap parsed;
    std::string error;
    Expect(brp::next::FlatJson::Parse(text, parsed, error), "serialize/parse succeeds");
    Expect(brp::next::FlatJson::GetString(parsed, "name", "") == "PS5 \"Living Room\"\\den",
           "string escaping round-trips");
    Expect(brp::next::FlatJson::GetInt(parsed, "fps", 0) == 120, "int round-trips");
    Expect(brp::next::FlatJson::GetBool(parsed, "hdr", false), "bool round-trips");
    Expect(Near(brp::next::FlatJson::GetDouble(parsed, "opacity", 0.0), 0.75, 1e-9),
           "double round-trips");

    brp::next::FlatMap legacy;
    const std::string json =
        "{ \"name\": \"PS5\", \"nested\": {\"a\": 1}, \"arr\": [1,2], \"fps\": 30 }";
    Expect(brp::next::FlatJson::Parse(json, legacy, error), "nested values are skipped");
    Expect(brp::next::FlatJson::GetInt(legacy, "fps", 0) == 30,
           "scalar after nested values is read");
}

void TestSettings() {
    std::cout << "Settings\n";
    brp::next::AppSettings settings;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    settings.Validate(warnings, errors);
    Expect(errors.empty(), "defaults validate");

    settings.streaming.fps = 10;
    settings.overlay.opacity = 2.0f;
    settings.Validate(warnings, errors);
    Expect(errors.size() >= 2, "invalid fps/opacity are errors");

    settings = brp::next::AppSettings{};
    settings.streaming.fps = 30;
    settings.ai.frame_generation = true;
    settings.ai.fg_backend = brp::next::AiBackend::DlssFg;
    warnings.clear();
    errors.clear();
    settings.Validate(warnings, errors);
    Expect(errors.empty(), "DLSS-G settings validate without a target");

    brp::next::FlatMap flat = brp::next::SettingsToFlat(settings);
    brp::next::AppSettings restored;
    brp::next::SettingsFromFlat(flat, restored);
    Expect(restored.streaming.fps == 30, "fps round-trips");
    Expect(restored.ai.fg_backend == brp::next::AiBackend::DlssFg,
           "backend round-trips");
    settings.ai.fg_multiplier = 4;
    brp::next::FlatMap mult_flat = brp::next::SettingsToFlat(settings);
    brp::next::AppSettings mult_restored;
    brp::next::SettingsFromFlat(mult_flat, mult_restored);
    Expect(mult_restored.ai.fg_multiplier == 4, "DLSS-G multiplier round-trips");
    mult_flat["ai.fg_multiplier"] = brp::next::FlatValue{"9", false};
    brp::next::SettingsFromFlat(mult_flat, mult_restored);
    Expect(mult_restored.ai.fg_multiplier == 6, "DLSS-G multiplier clamps to 6");

    // Frame generation is paired with its own upscaler. A mixed pair is
    // dropped while loading, not handed to the engine.
    brp::next::AppSettings mixed;
    mixed.ai.sr_model = "fsr4";
    mixed.ai.frame_generation = true;
    mixed.ai.fg_backend = brp::next::AiBackend::DlssFg;
    warnings.clear();
    errors.clear();
    mixed.Validate(warnings, errors);
    Expect(!warnings.empty(), "mixed FG/SR pair warns");
    brp::next::FlatMap mixed_flat = brp::next::SettingsToFlat(mixed);
    brp::next::AppSettings mixed_restored;
    brp::next::SettingsFromFlat(mixed_flat, mixed_restored);
    Expect(mixed_restored.ai.fg_backend == brp::next::AiBackend::None,
           "DLSS-G is dropped on FSR SR");
    Expect(!mixed_restored.ai.frame_generation,
           "frame generation is off after the mixed pair is dropped");

    brp::next::AiSettings pair;
    pair.sr_model = "fsr4";
    pair.fg_backend = brp::next::AiBackend::FsrFg;
    Expect(pair.FgMatchesSr(false), "FSR-G matches FSR SR");
    pair.fg_backend = brp::next::AiBackend::DlssFg;
    Expect(!pair.FgMatchesSr(false), "DLSS-G is rejected on FSR SR");
    pair.sr_model = "auto";
    Expect(!pair.FgMatchesSr(false), "auto on AMD resolves to FSR and rejects DLSS-G");
    Expect(pair.FgMatchesSr(true), "auto on NVIDIA resolves to DLSS");

    brp::next::AppSettings fs;
    fs.display.fullscreen_on_play = false;
    brp::next::FlatMap fs_flat = brp::next::SettingsToFlat(fs);
    brp::next::AppSettings fs_restored;
    brp::next::SettingsFromFlat(fs_flat, fs_restored);
    Expect(!fs_restored.display.fullscreen_on_play,
           "fullscreen-on-play round-trips");
    Expect(restored.general.theme == brp::next::Theme::Dark, "theme default is dark");

    flat["streaming.fps"] = brp::next::FlatValue{"999", false};
    flat["overlay.opacity"] = brp::next::FlatValue{"5.0", false};
    brp::next::SettingsFromFlat(flat, restored);
    Expect(restored.streaming.fps == 240, "fps clamps to 240");
    Expect(Near(restored.overlay.opacity, 1.0, 1e-6), "opacity clamps to 1.0");
}

void TestSettingsStore(const std::filesystem::path& root) {
    std::cout << "SettingsStore\n";
    const auto path = root / "settings.json";
    brp::next::SettingsStore store(path);
    brp::next::AppSettings settings;
    std::vector<std::string> warnings;
    Expect(store.Load(settings, warnings).ok(), "missing file loads defaults");
    settings.console.address = "192.168.1.21";
    settings.streaming.fps = 60;
    Expect(store.Save(settings).ok(), "save succeeds");

    brp::next::AppSettings loaded;
    warnings.clear();
    Expect(store.Load(loaded, warnings).ok(), "load succeeds");
    Expect(loaded.console.address == "192.168.1.21", "address persists");
    Expect(loaded.streaming.fps == 60, "fps persists");

    {
        std::ofstream broken(path, std::ios::trunc);
        broken << "{ not json";
    }
    const auto status = store.Load(loaded, warnings);
    Expect(!status.ok(), "broken JSON is reported");
}

void TestLegacyImport(const std::filesystem::path& root) {
    std::cout << "Legacy import\n";
    const auto path = root / "session.json";
    {
        std::ofstream file(path, std::ios::trunc);
        file << "{\n"
                "  \"name\": \"PS5\",\n"
                "  \"host\": \"192.168.1.21\",\n"
                "  \"width\": 1920,\n"
                "  \"height\": 1080,\n"
                "  \"fps\": 60,\n"
                "  \"neural_sr\": 1,\n"
                "  \"dlss_mode\": 5,\n"
                "  \"dlss_fg\": 2,\n"
                "  \"fg_engine\": 1,\n"
                "  \"fg_target\": 120,\n"
                "  \"rife_model\": 1,\n"
                "  \"rife_blend\": 20,\n"
                "  \"rife_safe_motion\": 0,\n"
                "  \"testing_log\": 1\n"
                "}\n";
    }
    brp::next::AppSettings imported;
    std::vector<std::string> warnings;
    const auto status = brp::next::ImportLegacySettings(path, imported, warnings);
    Expect(status.ok(), "legacy file imports");
    Expect(imported.console.address == "192.168.1.21", "host mapped");
    Expect(imported.streaming.input == brp::next::Resolution::R1080, "resolution mapped");
    Expect(imported.ai.super_resolution, "SR flag mapped");
    Expect(imported.ai.frame_generation, "FG flag mapped");
    Expect(imported.ai.fg_backend == brp::next::AiBackend::DlssFg,
           "legacy engine falls back to DLSS-G");
    Expect(imported.ai.fg_multiplier == 2, "DLSS-G multiplier mapped");
    Expect(imported.advanced.debug_log, "testing log mapped");
    Expect(!warnings.empty(), "import reports warnings");
}

void TestProfiles(const std::filesystem::path& root) {
    std::cout << "Profiles\n";
    brp::next::ProfileStore store(root / "profiles");
    brp::next::AppSettings settings;
    settings.streaming.fps = 60;
    settings.ai.super_resolution = true;

    brp::next::Profile created;
    Expect(store.Create("AI Ultra 4K", settings, created).ok(), "create profile");
    Expect(saved_id_valid(created.id), "profile id is safe");
    Expect(created.name == "AI Ultra 4K", "profile name kept");

    std::vector<brp::next::Profile> list;
    Expect(store.List(list).ok() && list.size() == 1, "list returns one profile");

    brp::next::Profile duplicate;
    Expect(store.Duplicate(created.id, "AI Ultra copy", duplicate).ok(),
           "duplicate profile");
    Expect(store.List(list).ok() && list.size() == 2, "duplicate is listed");
    Expect(duplicate.settings.ai.super_resolution, "duplicate carries settings");

    Expect(store.SetDefault(created.id).ok(), "set default");
    Expect(store.Get(created.id, duplicate).ok() && duplicate.is_default,
           "default flag persists");

    const auto export_path = root / "exported-profile.json";
    Expect(store.Export(created.id, export_path.string()).ok(), "export profile");
    brp::next::Profile imported;
    Expect(store.Import(export_path.string(), imported).ok(), "import profile");
    Expect(imported.name == "AI Ultra 4K", "import keeps name");
    Expect(store.Save(imported).ok(), "imported profile saves");

    Expect(store.Delete(created.id).ok(), "delete profile");
    Expect(store.List(list).ok() && list.size() == 2, "delete leaves imported + copy");
    bool has_default = false;
    for (const auto& profile : list) {
        has_default = has_default || profile.is_default;
    }
    Expect(has_default, "a default is reassigned after delete");
}

bool saved_id_valid(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

void TestNavigation() {
    std::cout << "Navigation\n";
    brp::next::NavigationModel nav;
    Expect(nav.Current() == brp::next::Page::Home, "starts at Home");
    Expect(nav.Select(brp::next::Page::AiVideo), "select AI page");
    Expect(nav.Current() == brp::next::Page::AiVideo, "AI is current");
    Expect(nav.CanGoBack(), "history has Home");
    Expect(nav.Back() && nav.Current() == brp::next::Page::Home, "back returns Home");
    Expect(!nav.Select(brp::next::Page::Home), "selecting current is a no-op");
    nav.SetCollapsed(true);
    Expect(nav.Collapsed(), "collapsed state");
    Expect(nav.Find(brp::next::Page::Diagnostics) != nullptr, "page metadata exists");
}

void TestMetrics(const std::filesystem::path& root) {
    std::cout << "Metrics\n";
    brp::next::DiagnosticsCollector diagnostics;
    for (int i = 1; i <= 100; ++i) {
        diagnostics.AddFrame(static_cast<double>(i), 1.0, 2.0, 3.0, 4.0);
    }
    diagnostics.Tick(10.0);
    diagnostics.AddDropped(2);
    const auto snapshot = diagnostics.Snapshot();
    Expect(Near(snapshot.frame_p50_ms, 50.0, 1.0), "p50 is ~50");
    Expect(Near(snapshot.frame_p99_ms, 99.0, 1.0), "p99 is ~99");
    Expect(Near(snapshot.decode_ms, 1.0, 1e-9), "decode mean");
    Expect(snapshot.dropped_frames == 2, "dropped counter");

    const auto report = root / "report.txt";
    Expect(diagnostics.WriteReport(report.string()).ok(), "write report");
    std::ifstream file(report);
    std::string contents((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    Expect(contents.find("frame_p99_ms=") != std::string::npos, "report has p99");
}

void TestTheme() {
    std::cout << "Theme\n";
    const auto& dark = brp::next::ThemeFor(brp::next::Theme::Dark);
    const auto& light = brp::next::ThemeFor(brp::next::Theme::Light);
    Expect(dark.accent.r == 0xB0 && dark.accent.g == 0x72 && dark.accent.b == 0xFF,
           "dark accent is the PS5 purple");
    Expect(dark.background.r < dark.surface.r, "dark background is darker than surface");
    Expect(light.background.r > light.text.r, "light background is lighter than text");
    Expect(dark.sidebar_width > dark.sidebar_collapsed, "sidebar collapse is narrower");
    Expect(dark.space_xs < dark.space_sm && dark.space_sm < dark.space_md,
           "spacing scale is monotonic");
}

void TestHost(const std::filesystem::path& root) {
    std::cout << "AppHost\n";
    const auto dir = root / "host";
    std::filesystem::create_directories(dir);
    {
        std::ofstream legacy(dir / "legacy.json", std::ios::trunc);
        legacy << "{ \"name\": \"PS5\", \"host\": \"192.168.1.21\", \"fps\": 60, "
                  "\"neural_sr\": 1, \"dlss_fg\": 2, \"fg_engine\": 0 }";
    }
    brp::next::AppPaths paths;
    paths.settings_file = dir / "settings.json";
    paths.profiles_dir = dir / "profiles";
    paths.log_file = dir / "host.log";
    paths.legacy_settings = dir / "legacy.json";

    brp::next::AppHost host(paths);
    brp::next::StartupReport report;
    const auto status = host.Initialize(report);
    Expect(status.ok(), "host initializes");
    Expect(report.legacy_imported, "legacy profile imported on first run");
    Expect(host.Settings().console.address == "192.168.1.21", "legacy address available");
    Expect(host.Services().Get<brp::next::IConfigStore>() != nullptr,
           "config service registered");
    Expect(host.Services().Get<brp::next::ILogger>() != nullptr,
           "logger service registered");
    Expect(host.Navigation().Current() == brp::next::Page::Home, "navigation starts Home");

    auto bad = host.Settings();
    bad.streaming.fps = 10;
    Expect(!host.ApplySettings(bad).ok(), "invalid settings are rejected");

    auto good = host.Settings();
    good.general.theme = brp::next::Theme::Light;
    Expect(host.ApplySettings(good).ok(), "valid settings apply");
    Expect(host.Settings().general.theme == brp::next::Theme::Light, "theme applied");

    Expect(host.ResetSettings().ok(), "reset to defaults");
    Expect(host.Settings().general.theme == brp::next::Theme::Dark, "reset restores dark");

    host.Shutdown();
    Expect(std::filesystem::exists(paths.log_file), "log file created");
}

}  // namespace

int main() {
    const auto root = TempRoot();
    std::cout << "Veyla foundation tests\n";
    TestFlatJson();
    TestSettings();
    TestSettingsStore(root);
    TestLegacyImport(root);
    TestProfiles(root);
    TestNavigation();
    TestTheme();
    TestHost(root);
    TestMetrics(root);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::cout << (g_failures == 0 ? std::string("ALL PASS")
                                  : "FAILURES: " + std::to_string(g_failures))
              << "\n";
    return g_failures == 0 ? 0 : 1;
}
