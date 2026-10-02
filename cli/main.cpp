// rec.exe: command-line front-end over rec_core (recorder plan §12, X12).
// Exit codes: 0 success, 1 error, 2 command not implemented yet.
#include <windows.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "file_commands.h"
#include "hook_commands.h"
#include "rec/config.h"
#include "rec/doctor.h"
#include "rec/log.h"
#include "rec/options.h"
#include "rec/paths.h"
#include "rec/version.h"

namespace {

const char* const kVersion = rec::kVersionLabel;

std::string u8(const std::filesystem::path& p) { return rec::to_utf8(p.wstring()); }

// Recording options shared by launch and attach (§12.2). Each maps onto a rec.toml key, so the
// command line goes through the same checks as the file.
struct RecordArgs {
    std::optional<int> fps, queue_mb, split_gb;
    std::optional<std::string> size, encoder, format, audio, mic, out, hotkey;
    bool lock = true, sound = true, force = false;
    int duration = 0, record_for = 0;
    std::string save_frame;
    int save_frame_index = 30;
    CLI::Option* lock_opt = nullptr;
    CLI::Option* sound_opt = nullptr;

    void add_to(CLI::App* cmd) {
        cmd->add_option("--fps", fps, "frame rate (default 60; capped at the display refresh rate)");
        cmd->add_option("--size", size, "output size WxH or native (default 1280x720)");
        lock_opt = cmd->add_flag("--lock,!--no-lock", lock, "lock the game to the recording frame rate");
        cmd->add_option("--encoder", encoder, "rcv | rcv-strict | hw");
        cmd->add_option("--format", format, "yuv420 | rgb");
        cmd->add_option("--audio", audio, "game | system | none");
        cmd->add_option("--mic", mic, "microphone device name (separate track)");
        cmd->add_option("--out", out, "output folder");
        cmd->add_option("--hotkey", hotkey, "start/stop key, e.g. F9 or Ctrl+Shift+R");
        sound_opt = cmd->add_flag("--sound,!--no-sound", sound, "start/stop sound cue");
        cmd->add_option("--queue-mb", queue_mb, "packet queue size in MB");
        cmd->add_option("--split-gb", split_gb, "split files every N GB (0 = only on FAT32)");
        cmd->add_flag("--force", force, "hook even if anti-cheat is found (only for games you own and know are safe offline)");
        cmd->add_option("--duration", duration, "detach and exit after this many seconds (0 = until Ctrl+C)")->check(CLI::NonNegativeNumber);
        cmd->add_option("--record-for", record_for, "record this many seconds as soon as the game presents, then exit (for scripts)")
            ->check(CLI::NonNegativeNumber)
            ->group("");
        cmd->add_option("--save-frame", save_frame, "write one captured frame of the recording as a PNG (test aid)")->group("");
        cmd->add_option("--save-frame-index", save_frame_index, "which captured frame to save (0-based, default 30)")->check(CLI::NonNegativeNumber)->group("");
    }

    // Applies the given options onto cfg; false with a message on the first invalid one.
    bool apply(rec::Config* cfg, std::string* error) const {
        auto set = [&](const char* key, const std::string& value) { return rec::set_config_value(cfg, key, value, error); };
        auto quoted = [](const std::string& s) { return "'" + s + "'"; };  // TOML literal string
        if (fps && !set("record.fps", std::to_string(*fps))) return false;
        if (size && !set("record.size", quoted(*size))) return false;
        if (lock_opt && lock_opt->count() && !set("record.lock", lock ? "true" : "false")) return false;
        if (encoder && !set("record.encoder", quoted(*encoder))) return false;
        if (format && !set("record.format", quoted(*format))) return false;
        if (audio && !set("audio.source", quoted(*audio))) return false;
        if (mic && !set("audio.mic", quoted(*mic))) return false;
        if (out && !set("record.out_dir", quoted(*out))) return false;
        if (hotkey && !set("hotkeys.toggle", quoted(*hotkey))) return false;
        if (sound_opt && sound_opt->count() && !set("hotkeys.sound", sound ? "true" : "false")) return false;
        if (queue_mb && !set("record.queue_mb", std::to_string(*queue_mb))) return false;
        if (split_gb && !set("record.split_gb", std::to_string(*split_gb))) return false;
        return true;
    }
};

void print_record_settings(const rec::Config& c) {
    std::printf("  %s @ %d fps, %s, encoder %s, %s, audio %s%s, hotkey %s%s\n  output: %s\n", c.record.size.c_str(),
                c.record.fps, c.record.lock ? "locked" : "not locked", c.record.encoder.c_str(), c.record.format.c_str(),
                c.audio.source.c_str(), c.audio.mic.empty() ? "" : (" + mic \"" + c.audio.mic + "\"").c_str(),
                c.hotkeys.toggle.c_str(), c.hotkeys.sound ? " (with sound cue)" : "",
                u8(rec::expand_env(c.record.out_dir)).c_str());
}

// A failed command: to the console and to the application log.
int fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    rec::logging::get(rec::Subsystem::Cli).warn("{}", message);
    return 1;
}

int not_yet(const char* command, const char* milestone) {
    std::fprintf(stderr, "rec %s: not implemented yet; it arrives in recorder milestone %s.\n", command, milestone);
    rec::logging::get(rec::Subsystem::Cli).info("rec {} is not implemented yet ({})", command, milestone);
    return 2;
}

int run_doctor(const rec::Config& cfg, const std::filesystem::path& config_path) {
    std::printf("rec doctor  (config: %s)\n\n", u8(config_path).c_str());
    const std::vector<rec::CheckResult> results = rec::run_doctor(cfg);
    int counts[3] = {};
    for (const rec::CheckResult& r : results) {
        static const char* const kStatus[] = {"PASS", "WARN", "FAIL"};
        const int s = int(r.status);
        ++counts[s];
        std::printf("  %s  %-14s %s\n", kStatus[s], r.name.c_str(), r.detail.c_str());
        if (r.status != rec::CheckStatus::Pass && !r.fix.empty()) std::printf("        -> %s\n", r.fix.c_str());
        rec::logging::get(rec::Subsystem::Cli).debug("doctor {} {}: {}", kStatus[s], r.name, r.detail);
    }
    std::printf("\n%d passed, %d warning(s), %d failed\n", counts[0], counts[1], counts[2]);
    rec::logging::event(rec::Ev::DoctorResult, "pass={} warn={} fail={}", counts[0], counts[1], counts[2]);
    return counts[2] ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    CLI::App app{"rec - frame-buffer-hook screen recorder", "rec"};
    argv = app.ensure_utf8(argv);
    app.set_version_flag("--version", kVersion);
    bool verbose = false;
    std::string config_file;
    app.add_flag("-v,--verbose", verbose, "DEBUG messages in the log files");
    app.add_option("--config", config_file, "configuration file (default %LOCALAPPDATA%\\rec\\rec.toml)");
    app.require_subcommand(1);

    // Commands (§12.1).
    auto* list = app.add_subcommand("list", "processes with graphics APIs loaded");

    RecordArgs launch_args, attach_args;
    std::string exe;
    std::vector<std::string> game_args;
    auto* launch = app.add_subcommand("launch", "start a game with the recorder hooked in");
    launch->add_option("exe", exe, "game executable")->required();
    launch->add_option("game_args", game_args, "arguments for the game (after --)");
    launch_args.add_to(launch);

    std::optional<unsigned> pid;
    std::optional<std::string> name;
    auto* attach = app.add_subcommand("attach", "hook a running game");
    auto* pid_opt = attach->add_option("--pid", pid, "process id");
    attach->add_option("--name", name, "process name, e.g. javaw.exe")->excludes(pid_opt);
    attach_args.add_to(attach);

    auto* detach = app.add_subcommand("detach", "unhook the game (default: every game that has the hook)");
    detach->add_option("--pid", pid, "process id");
    detach->add_option("--name", name, "process name");

    std::string bench_path, bench_size = "2GB";
    auto* bench_disk = app.add_subcommand("bench-disk", "measure sustained disk write speed");
    bench_disk->add_option("--path", bench_path, "folder to test (default: output folder)");
    bench_disk->add_option("--size", bench_size, "amount to write");

    int overhead_seconds = 60;
    auto* bench_overhead = app.add_subcommand("bench-overhead", "measure the recorder's effect on game frame times");
    bench_overhead->add_option("--pid", pid, "process id");
    bench_overhead->add_option("--name", name, "process name");
    bench_overhead->add_option("--seconds", overhead_seconds, "length of each window");

    std::string in_file, convert_to = "mp4", convert_out;
    int crf = 16;
    auto* convert = app.add_subcommand("convert", "decode a recording and encode it with FFmpeg");
    convert->add_option("in", in_file, "recording (.avi)")->required();
    convert->add_option("--to", convert_to, "mp4 | mkv");
    convert->add_option("--crf", crf, "x264 quality");
    convert->add_option("--out", convert_out, "output file");

    bool verify_testapp = false;
    std::string verify_source;
    auto* verify = app.add_subcommand("verify", "decode every frame and check the file structure");
    verify->add_option("in", in_file, "recording (.avi)")->required();
    verify->add_flag("--testapp", verify_testapp, "also read rec_testapp's frame-counter barcode and check continuity");
    verify->add_option("--source", verify_source, "the game's window size for the barcode, e.g. 1366x745 (default: from the file)");
    auto* repair = app.add_subcommand("repair", "rebuild a recording's indexes after a crash");
    repair->add_option("in", in_file, "recording (.avi)")->required();

    auto* doctor = app.add_subcommand("doctor", "check this PC for recording");

    std::string config_key, config_value;
    auto* config = app.add_subcommand("config", "show or change rec.toml");
    config->require_subcommand(1);
    auto* config_show = config->add_subcommand("show", "print every setting");
    auto* config_set = config->add_subcommand("set", "change one setting, e.g. rec config set record.fps 50");
    config_set->add_option("key", config_key, "section.key")->required();
    config_set->add_option("value", config_value, "new value")->required();
    auto* config_reset = config->add_subcommand("reset", "restore every default");

    CLI11_PARSE(app, argc, argv);

    // Configuration, then logging (the log level can come from the file).
    const std::filesystem::path config_path =
        config_file.empty() ? rec::default_config_path() : std::filesystem::path(rec::from_utf8(config_file));
    rec::Config cfg;
    std::vector<std::string> problems;
    const bool config_ok = rec::load_config(config_path, &cfg, &problems);
    for (const std::string& p : problems) std::fprintf(stderr, "rec.toml: %s\n", p.c_str());

    std::string error;
    rec::logging::Options log_options;
    log_options.app_log_dir = rec::app_log_dir();
    log_options.verbose = verbose || cfg.log.level == "debug";
    if (!rec::logging::start(log_options, &error)) std::fprintf(stderr, "warning: %s\n", error.c_str());
    std::string command_line;
    for (int i = 1; i < argc; ++i) command_line += std::string(i > 1 ? " " : "") + argv[i];
    rec::logging::event(rec::Ev::CommandStart, "rec {} (version {})", command_line, kVersion);
    for (const std::string& p : problems) rec::logging::get(rec::Subsystem::Cli).warn("rec.toml: {}", p);

    int status = 0;
    if (!config_ok && !config_reset->parsed()) {
        status = fail("Fix " + u8(config_path) + ", or run: rec config reset");
    } else if (*doctor) {
        status = run_doctor(cfg, config_path);
    } else if (*config_show) {
        std::error_code ec;
        std::printf("# %s%s\n%s", u8(config_path).c_str(),
                    std::filesystem::exists(config_path, ec) ? "" : " (not created yet: these are the defaults)",
                    rec::config_to_toml(cfg).c_str());
    } else if (*config_set) {
        if (!rec::set_config_value(&cfg, config_key, config_value, &error) || !rec::save_config(config_path, cfg, &error)) {
            status = fail("rec config set: " + error);
        } else {
            rec::logging::event(rec::Ev::ConfigChanged, "{} = {}", config_key, config_value);
            std::printf("%s = %s  (saved to %s)\n", config_key.c_str(), config_value.c_str(), u8(config_path).c_str());
        }
    } else if (*config_reset) {
        if (!rec::save_config(config_path, rec::Config{}, &error)) {
            status = fail("rec config reset: " + error);
        } else {
            rec::logging::event(rec::Ev::ConfigChanged, "reset to defaults");
            std::printf("Defaults written to %s\n", u8(config_path).c_str());
        }
    } else if (*launch || *attach) {
        const RecordArgs& ra = *launch ? launch_args : attach_args;
        if (*attach && !pid && !name) {
            status = fail("rec attach: give --pid N or --name X.exe");
        } else if (!ra.apply(&cfg, &error)) {
            status = fail(std::string("rec ") + (*launch ? "launch" : "attach") + ": " + error);
        } else {
            std::printf("Recording settings:\n");
            print_record_settings(cfg);
            rec_cli::HookCommandOptions options;
            options.force = ra.force;
            options.duration_s = ra.duration;
            options.record_for_s = ra.record_for;
            options.save_frame = ra.save_frame;
            options.save_frame_index = uint64_t(ra.save_frame_index);
            status = *launch ? rec_cli::cmd_launch(cfg, exe, game_args, options) : rec_cli::cmd_attach(cfg, pid, name, options);
        }
    } else if (*list) {
        status = rec_cli::cmd_list();
    } else if (*detach) {
        status = rec_cli::cmd_detach(pid, name);
    } else if (*bench_disk) {
        status = not_yet("bench-disk", "M5");
    } else if (*bench_overhead) {
        status = not_yet("bench-overhead", "M4");
    } else if (*convert) {
        status = rec_cli::cmd_convert(in_file, convert_to, crf, convert_out);
    } else if (*verify) {
        status = rec_cli::cmd_verify(in_file, verify_testapp, verify_source);
    } else if (*repair) {
        status = not_yet("repair", "M7");
    }
    rec::logging::stop();
    return status;
}
