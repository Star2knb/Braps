#include "rec/hooklink.h"

#include <cstring>
#include <string_view>

#include "rec/log.h"
#include "rec/paths.h"

namespace rec {
namespace {

uint64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

char level_letter(Level l) {
    switch (l) {
    case Level::Debug: return 'D';
    case Level::Info: return 'I';
    case Level::Warn: return 'W';
    case Level::Error: return 'E';
    case Level::Fatal: return 'F';
    }
    return '?';
}

void log_at(Level level, std::string_view text) {
    spdlog::logger& l = logging::get(Subsystem::Hook);
    switch (level) {
    case Level::Debug: l.debug("{}", text); break;
    case Level::Info: l.info("{}", text); break;
    case Level::Warn: l.warn("{}", text); break;
    case Level::Error: l.error("{}", text); break;
    case Level::Fatal: l.critical("{}", text); break;
    }
}

}  // namespace

const char* hook_state_name(proto::HookState state) {
    switch (state) {
    case proto::HookState::None: return "not loaded";
    case proto::HookState::Loading: return "loading";
    case proto::HookState::Waiting: return "waiting for the graphics DLLs";
    case proto::HookState::Hooked: return "hooked";
    case proto::HookState::Failed: return "failed";
    case proto::HookState::Idle: return "idle (host lost)";
    case proto::HookState::Detaching: return "detaching";
    case proto::HookState::Detached: return "detached";
    }
    return "?";
}

HookLink::~HookLink() {
    if (ctl_) UnmapViewOfFile(ctl_);
    if (ring_) UnmapViewOfFile(ring_);
    if (ctl_map_) CloseHandle(ctl_map_);
    if (log_map_) CloseHandle(log_map_);
    if (cmd_event_) CloseHandle(cmd_event_);
}

bool HookLink::map(bool create, std::string* error, bool* existed) {
    wchar_t name[64];
    *existed = false;
    auto open_one = [&](const wchar_t* suffix, size_t size, HANDLE* handle, void** view) {
        proto::object_name(name, 64, pid_, suffix);
        if (create) {
            *handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(size), name);
            if (*handle && GetLastError() == ERROR_ALREADY_EXISTS) *existed = true;
        } else {
            *handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        }
        if (!*handle) return false;
        *view = MapViewOfFile(*handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
        return *view != nullptr;
    };
    void* ctl_view = nullptr;
    void* log_view = nullptr;
    if (!open_one(L"ctl", sizeof(proto::ControlBlock), &ctl_map_, &ctl_view) ||
        !open_one(L"log", sizeof(proto::LogRing), &log_map_, &log_view)) {
        *error = create ? "can't create the shared memory: " + win32_error_text(GetLastError())
                        : "no recorder hook is attached to process " + std::to_string(pid_);
        return false;
    }
    ctl_ = static_cast<proto::ControlBlock*>(ctl_view);
    ring_ = static_cast<proto::LogRing*>(log_view);

    proto::object_name(name, 64, pid_, L"cmd");
    cmd_event_ = create ? CreateEventW(nullptr, FALSE, FALSE, name) : OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (!cmd_event_) {
        *error = "can't open the command event: " + win32_error_text(GetLastError());
        return false;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_freq_ = f.QuadPart;
    return true;
}

std::unique_ptr<HookLink> HookLink::open_or_create(uint32_t game_pid, std::string* error, bool* took_over) {
    std::unique_ptr<HookLink> link(new HookLink);
    link->pid_ = game_pid;
    bool existed = false;
    if (!link->map(true, error, &existed)) return nullptr;
    if (took_over) *took_over = existed;
    proto::ControlBlock* c = link->ctl_;
    if (existed) {
        // An earlier host's objects (its hook is still loaded): check they are ours, then take over.
        if (c->magic != proto::kMagic || c->version != proto::kVersion || c->size != sizeof(proto::ControlBlock) || c->game_pid != game_pid) {
            *error = "shared memory for process " + std::to_string(game_pid) + " exists but is not compatible (another rec version?)";
            return nullptr;
        }
        link->previous_host_ = c->host_pid.load();
        c->attach_count.fetch_add(1);
    } else {
        c->magic = proto::kMagic;
        c->version = proto::kVersion;
        c->size = sizeof(proto::ControlBlock);
        c->game_pid = game_pid;
        c->qpc_frequency = link->qpc_freq_;
        c->attach_count.store(1);
        link->ring_->init();
    }
    c->host_pid.store(GetCurrentProcessId());
    link->heartbeat();
    return link;
}

std::unique_ptr<HookLink> HookLink::open_existing(uint32_t game_pid, std::string* error) {
    std::unique_ptr<HookLink> link(new HookLink);
    link->pid_ = game_pid;
    bool existed = false;
    if (!link->map(false, error, &existed)) return nullptr;
    proto::ControlBlock* c = link->ctl_;
    if (c->magic != proto::kMagic || c->version != proto::kVersion || c->size != sizeof(proto::ControlBlock) || c->game_pid != game_pid) {
        *error = "shared memory for process " + std::to_string(game_pid) + " is not compatible";
        return nullptr;
    }
    return link;
}

void HookLink::heartbeat() { ctl_->host_heartbeat_qpc.store(qpc_now(), std::memory_order_relaxed); }

int HookLink::drain_log() {
    int n = 0;
    proto::LogRecord r;
    while (ring_->pop(&r)) {
        ++n;
        const Level level = r.level <= uint8_t(Level::Fatal) ? Level(r.level) : Level::Info;
        std::string text;
        if (r.flags & proto::kLogText) {
            const char* chars = reinterpret_cast<const char*>(r.arg);
            text.assign(chars, strnlen(chars, proto::kLogTextMax));
        } else {
            text = "a0=" + std::to_string(r.arg[0]) + " a1=" + std::to_string(r.arg[1]) + " a2=" + std::to_string(r.arg[2]) +
                   " a3=" + std::to_string(r.arg[3]);
        }
        if (r.code != 0) {
            char code[8];
            std::snprintf(code, sizeof(code), "%c%04u", level_letter(level), unsigned(r.code));
            if (const EventInfo* info = find_event(code)) {
                logging::event(info->id, text);
            } else {
                log_at(level, std::string(code) + " " + text);
            }
        } else {
            log_at(level, text);
        }
    }
    const uint64_t dropped = ring_->dropped.load(std::memory_order_relaxed);
    if (dropped != reported_dropped_) {
        logging::event(Ev::HookLogOverflow, "{} records lost", dropped - reported_dropped_);
        reported_dropped_ = dropped;
    }
    return n;
}

HookStats HookLink::sample() {
    proto::ControlBlock* c = ctl_;
    HookStats s;
    s.state = proto::HookState(c->hook_state.load(std::memory_order_acquire));
    s.backend = c->backend.load();
    s.hooked_apis = c->hooked_apis.load();
    s.width = c->backbuffer_width.load();
    s.height = c->backbuffer_height.load();
    s.format = c->backbuffer_format.load();
    s.error_code = c->error_code.load();
    s.ignored = c->present_ignored.load();
    s.hook_cost_max_us = double(c->hook_cost_max_ns.exchange(0)) / 1000.0;

    const uint64_t now = qpc_now();
    const uint64_t beat = c->hook_heartbeat_qpc.load();
    s.hook_alive = beat != 0 && now >= beat && (now - beat) < 2 * uint64_t(qpc_freq_);

    Point p{now, c->present_count.load(), c->hook_cost_total_ns.load()};
    s.present_count = p.count;
    // Frame rate over the oldest sample that is at least one second old (or the oldest we have).
    const Point* base = nullptr;
    for (int i = 0; i < history_size_; ++i)
        if (now - history_[i].qpc >= uint64_t(qpc_freq_)) base = &history_[i];
    if (!base && history_size_ && now - history_[0].qpc >= uint64_t(qpc_freq_) / 2) base = &history_[0];  // not before half a second
    if (base && p.qpc > base->qpc && p.count >= base->count) {
        const double seconds = double(p.qpc - base->qpc) / double(qpc_freq_);
        s.fps = double(p.count - base->count) / seconds;
    }
    if (history_size_) {
        const Point& last = history_[history_size_ - 1];
        if (p.count > last.count) s.hook_cost_us = double(p.cost_total - last.cost_total) / double(p.count - last.count) / 1000.0;
    }
    if (history_size_ == int(std::size(history_))) {
        std::memmove(history_, history_ + 1, sizeof(Point) * (std::size(history_) - 1));
        --history_size_;
    }
    history_[history_size_++] = p;
    return s;
}

void HookLink::request_detach() {
    ctl_->command.store(uint32_t(proto::Command::Detach));
    SetEvent(cmd_event_);
}

}  // namespace rec
