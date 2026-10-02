// rec.toml (recorder plan §17) and value parsing (§12.2, §13.1).
#include <windows.h>

#include "rec/config.h"
#include "rec/options.h"
#include "test_util.h"
#include "testfw.h"

using namespace rec;

namespace {

bool has_problem(const std::vector<std::string>& v, const std::string& part) {
    for (const std::string& s : v)
        if (s.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("config: defaults are the plan's and pass validation") {
    const Config c;
    CHECK(validate_config(c).empty());
    CHECK(c.record.fps == 60);
    CHECK(c.record.size == "1280x720");
    CHECK(c.hotkeys.toggle == "F9");
    CHECK(c.rate.levels.size() == 4);
    CHECK(c.safety.anticheat_blocklist.size() == 5);
    CHECK(config_keys().size() == 25);
}

TEST_CASE("config: save and load round trip, missing file gives defaults") {
    const rt::TempDir dir("config_rt");
    const auto path = dir.path() / "sub" / "rec.toml";
    Config c;
    std::vector<std::string> problems;
    CHECK(load_config(path, &c, &problems));  // missing: defaults
    CHECK(problems.empty());

    std::string err;
    REQUIRE(set_config_value(&c, "record.fps", "50", &err));
    REQUIRE(set_config_value(&c, "record.size", "1920x1080", &err));
    REQUIRE(set_config_value(&c, "record.out_dir", "D:\\Rec ü", &err));
    REQUIRE(set_config_value(&c, "rate.levels", "[30, 50, 70, 95]", &err));
    REQUIRE(set_config_value(&c, "log.slow_hook_ms", "2", &err));  // an integer is accepted for a number
    REQUIRE(set_config_value(&c, "hotkeys.sound", "false", &err));
    REQUIRE(set_config_value(&c, "safety.anticheat_blocklist", "[\"vgk\"]", &err));
    REQUIRE(save_config(path, c, &err));

    Config d;
    problems.clear();
    REQUIRE(load_config(path, &d, &problems));
    CHECK(problems.empty());
    CHECK(d.record.fps == 50);
    CHECK(d.record.size == "1920x1080");
    CHECK(d.record.out_dir == "D:\\Rec ü");
    CHECK(d.rate.levels == std::vector<int>({30, 50, 70, 95}));
    CHECK(d.log.slow_hook_ms == 2.0);
    CHECK(!d.hotkeys.sound);
    CHECK(d.safety.anticheat_blocklist == std::vector<std::string>({"vgk"}));
    CHECK(config_to_toml(d) == config_to_toml(c));
}

TEST_CASE("config: set_config_value checks keys, types and ranges") {
    Config c;
    std::string err;
    CHECK(!set_config_value(&c, "record.nope", "1", &err));
    CHECK(err.find("unknown key") != std::string::npos);
    CHECK(!set_config_value(&c, "record.fps", "fast", &err));
    CHECK(!set_config_value(&c, "record.fps", "0", &err));
    CHECK(!set_config_value(&c, "record.fps", "1.5", &err));
    CHECK(!set_config_value(&c, "record.lock", "yes", &err));
    CHECK(!set_config_value(&c, "record.size", "1281x720", &err));  // odd width
    CHECK(!set_config_value(&c, "record.encoder", "x264", &err));
    CHECK(!set_config_value(&c, "rate.levels", "[90, 60, 75, 40]", &err));
    CHECK(!set_config_value(&c, "hotkeys.toggle", "F25", &err));
    CHECK(!set_config_value(&c, "hotkeys.marker_key", "F9", &err));  // same as the toggle key
    CHECK(!set_config_value(&c, "log.very_slow_write_ms", "10", &err));  // below slow_write_ms
    CHECK(validate_config(c).empty());  // nothing above changed c
    CHECK(c.record.fps == 60);
    CHECK(set_config_value(&c, "record.size", "native", &err));
    CHECK(set_config_value(&c, "hotkeys.marker_key", "ctrl+f10", &err));
    CHECK(set_config_value(&c, "record.encoder", "\"hw\"", &err));
    CHECK(c.record.encoder == "hw");
}

TEST_CASE("config: bad entries in the file are reported and the defaults kept") {
    const rt::TempDir dir("config_bad");
    const auto path = dir.path() / "rec.toml";
    rt::write_file(path,
                   "[record]\nfps = \"sixty\"\nsize = \"640x480\"\nqueue_mb = 1\nbogus = 1\n"
                   "[hotkeys]\ntoggle = \"F10\"\n[extra]\nx = 1\n");
    Config c;
    std::vector<std::string> problems;
    REQUIRE(load_config(path, &c, &problems));
    CHECK(c.record.fps == 60);           // wrong type
    CHECK(c.record.size == "640x480");   // fine
    CHECK(c.record.queue_mb == 256);     // out of range
    CHECK(c.hotkeys.toggle == "F10");
    CHECK(has_problem(problems, "record.fps"));
    CHECK(has_problem(problems, "record.queue_mb"));
    CHECK(has_problem(problems, "record.bogus"));
    CHECK(has_problem(problems, "extra.x"));
    CHECK(problems.size() == 4);

    rt::write_file(path, "[record\nfps = 60\n");  // not TOML
    problems.clear();
    Config d;
    CHECK(!load_config(path, &d, &problems));
    CHECK(problems.size() == 1);
    CHECK(d.record.fps == 60);
}

TEST_CASE("options: output sizes") {
    OutputSize s;
    CHECK(parse_size("1280x720", &s));
    CHECK(!s.native);
    CHECK(s.width == 1280);
    CHECK(s.height == 720);
    CHECK(parse_size("NATIVE", &s));
    CHECK(s.native);
    CHECK(parse_size("16X16", &s));
    CHECK(!parse_size("1281x720", &s));
    CHECK(!parse_size("14x14", &s));
    CHECK(!parse_size("8194x720", &s));
    CHECK(!parse_size("1280", &s));
    CHECK(!parse_size("1280x", &s));
    CHECK(!parse_size("x720", &s));
    CHECK(!parse_size("-1280x720", &s));
    CHECK(!parse_size("", &s));
}

TEST_CASE("options: hotkeys") {
    Hotkey k;
    CHECK(parse_hotkey("F9", &k));
    CHECK(k.vk == VK_F9);
    CHECK(k.modifiers == 0);
    CHECK(parse_hotkey("ctrl+shift+f10", &k));
    CHECK(k.vk == VK_F10);
    CHECK(k.modifiers == (MOD_CONTROL | MOD_SHIFT));
    CHECK(hotkey_name(k) == "Ctrl+Shift+F10");
    CHECK(parse_hotkey("Alt+r", &k));
    CHECK(hotkey_name(k) == "Alt+R");
    CHECK(parse_hotkey("Win+PageUp", &k));
    CHECK(hotkey_name(k) == "Win+PageUp");
    CHECK(parse_hotkey("F24", &k));
    CHECK(!parse_hotkey("F25", &k));
    CHECK(!parse_hotkey("F0", &k));
    CHECK(!parse_hotkey("Ctrl", &k));
    CHECK(!parse_hotkey("Ctrl+", &k));
    CHECK(!parse_hotkey("Ctrl++F9", &k));
    CHECK(!parse_hotkey("F9+F10", &k));
    CHECK(!parse_hotkey("Hyper+F9", &k));
    CHECK(!parse_hotkey("", &k));
}
