// The host side of hooking (recorder plan §4, §7): shared-memory protocol and log ring, host link,
// anti-cheat matching, graphics module classification. The hook DLL itself is tested end to end by
// tests/m1_attach_detach.ps1 against rec_testapp.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "rec/anticheat.h"
#include "rec/hooklink.h"
#include "rec/log.h"
#include "rec/paths.h"
#include "rec/procscan.h"
#include "rec/protocol.h"
#include "testfw.h"

using namespace rec;

namespace {

proto::LogRecord make_record(uint64_t n) {
    proto::LogRecord r{};
    r.qpc = n;
    r.level = uint8_t(Level::Info);
    r.arg[0] = n;
    return r;
}

// The hook's side of a link: maps the objects the host created, as the hook DLL does.
template <class T>
T* map_hook_side(uint32_t pid, const wchar_t* suffix, size_t size) {
    wchar_t name[64];
    proto::object_name(name, 64, pid, suffix);
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!m) return nullptr;
    void* view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    CloseHandle(m);
    return static_cast<T*>(view);
}

// A pid that no game has; only used to name the objects.
constexpr uint32_t kFakePid = 0x7F000001;

}  // namespace

TEST_CASE("protocol: object names") {
    wchar_t name[64];
    proto::object_name(name, 64, 4242, L"ctl");
    CHECK(std::wstring(name) == L"Local\\rec_4242_ctl");
    proto::object_name(name, 64, 0, L"log");
    CHECK(std::wstring(name) == L"Local\\rec_0_log");
    wchar_t tiny[8];
    proto::object_name(tiny, 8, 123456, L"ctl");  // too small: truncated, still terminated
    CHECK(wcslen(tiny) < 8);
}

TEST_CASE("log ring: order, wrap-around and overflow") {
    auto ring = std::make_unique<proto::LogRing>();
    ring->init();
    proto::LogRecord out{};
    CHECK(!ring->pop(&out));
    for (uint64_t i = 0; i < 10; ++i) CHECK(ring->push(make_record(i)));
    for (uint64_t i = 0; i < 10; ++i) {
        REQUIRE(ring->pop(&out));
        CHECK(out.arg[0] == i);
    }
    // Several times round the ring.
    for (uint64_t round = 0; round < 3; ++round) {
        for (uint64_t i = 0; i < proto::kLogCapacity; ++i) CHECK(ring->push(make_record(i)));
        for (uint64_t i = 0; i < proto::kLogCapacity; ++i) {
            REQUIRE(ring->pop(&out));
            CHECK(out.arg[0] == i);
        }
    }
    // Full: the record is dropped and counted, never waited for.
    for (uint64_t i = 0; i < proto::kLogCapacity; ++i) CHECK(ring->push(make_record(i)));
    CHECK(!ring->push(make_record(999)));
    CHECK(!ring->push(make_record(999)));
    CHECK(ring->dropped.load() == 2);
    REQUIRE(ring->pop(&out));
    CHECK(out.arg[0] == 0);
    CHECK(ring->push(make_record(1000)));  // room again
}

TEST_CASE("log ring: many producers, one consumer, nothing lost or duplicated") {
    auto ring = std::make_unique<proto::LogRing>();
    ring->init();
    constexpr int kProducers = 4;
    constexpr uint64_t kEach = 20000;
    std::atomic<bool> done{false};
    std::vector<std::thread> producers;
    std::atomic<uint64_t> dropped{0};
    for (int p = 0; p < kProducers; ++p)
        producers.emplace_back([&, p] {
            for (uint64_t i = 0; i < kEach; ++i)
                if (!ring->push(make_record(uint64_t(p) * kEach + i))) dropped.fetch_add(1);
        });
    std::set<uint64_t> seen;
    uint64_t duplicates = 0;
    proto::LogRecord out{};
    auto drain = [&] {
        while (ring->pop(&out))
            if (!seen.insert(out.arg[0]).second) ++duplicates;
    };
    std::thread consumer([&] {
        while (!done.load()) drain();
    });
    for (auto& t : producers) t.join();
    done = true;
    consumer.join();
    drain();
    CHECK(duplicates == 0);
    CHECK(seen.size() + dropped.load() == kProducers * kEach);
    CHECK(ring->dropped.load() == dropped.load());
}

TEST_CASE("host link: create, hook-side access, log draining, statistics, takeover") {
    std::string error;
    bool took_over = true;
    std::unique_ptr<HookLink> link = HookLink::open_or_create(kFakePid, &error, &took_over);
    REQUIRE(link != nullptr);
    CHECK(!took_over);

    auto* ctl = map_hook_side<proto::ControlBlock>(kFakePid, L"ctl", sizeof(proto::ControlBlock));
    auto* ring = map_hook_side<proto::LogRing>(kFakePid, L"log", sizeof(proto::LogRing));
    REQUIRE(ctl != nullptr);
    REQUIRE(ring != nullptr);
    CHECK(ctl->magic == proto::kMagic);
    CHECK(ctl->version == proto::kVersion);
    CHECK(ctl->game_pid == kFakePid);
    CHECK(ctl->host_pid.load() == GetCurrentProcessId());
    CHECK(ctl->host_heartbeat_qpc.load() != 0);

    // A hook log record with an event code and text becomes that event in the host's counters.
    const uint64_t locate_failed = logging::event_count(Ev::LocateFailed);
    const uint64_t vtable = logging::event_count(Ev::VtablePointerOutsideModule);
    proto::LogRecord r{};
    r.level = uint8_t(Level::Error);
    r.flags = proto::kLogText;
    r.code = 1105;
    std::memcpy(r.arg, "D3D11 module not loaded", 24);
    CHECK(ring->push(r));
    r.level = uint8_t(Level::Warn);
    r.code = 1106;
    CHECK(ring->push(r));
    r.level = uint8_t(Level::Info);
    r.code = 0;  // plain text
    CHECK(ring->push(r));
    CHECK(link->drain_log() == 3);
    CHECK(logging::event_count(Ev::LocateFailed) == locate_failed + 1);
    CHECK(logging::event_count(Ev::VtablePointerOutsideModule) == vtable + 1);
    CHECK(link->drain_log() == 0);

    // Statistics: the frame rate comes from the Present counter.
    ctl->hook_state.store(uint32_t(proto::HookState::Hooked));
    ctl->backend.store(proto::kApiD3D11);
    ctl->backbuffer_width.store(1280);
    ctl->backbuffer_height.store(720);
    link->sample();
    Sleep(600);
    ctl->present_count.fetch_add(36);
    ctl->hook_cost_total_ns.fetch_add(36 * 2000);
    ctl->hook_cost_max_ns.store(5000);
    const HookStats s = link->sample();
    CHECK(s.state == proto::HookState::Hooked);
    CHECK(s.backend == proto::kApiD3D11);
    CHECK(s.width == 1280 && s.height == 720);
    CHECK(s.fps > 40 && s.fps < 90);  // 36 presents in about 0.6 s
    CHECK(s.hook_cost_us > 1.9 && s.hook_cost_us < 2.1);
    CHECK(s.hook_cost_max_us > 4.9 && s.hook_cost_max_us < 5.1);
    CHECK(link->sample().hook_cost_max_us == 0);  // the maximum is read and reset

    // Detach request reaches the hook's side.
    link->request_detach();
    CHECK(ctl->command.load() == uint32_t(proto::Command::Detach));

    // A second host takes over the existing objects (the hook of an earlier host is still loaded).
    bool second_took_over = false;
    std::unique_ptr<HookLink> second = HookLink::open_or_create(kFakePid, &error, &second_took_over);
    REQUIRE(second != nullptr);
    CHECK(second_took_over);
    CHECK(second->previous_host_pid() == GetCurrentProcessId());
    CHECK(ctl->attach_count.load() == 2);

    // rec detach opens existing objects only.
    CHECK(HookLink::open_existing(kFakePid, &error) != nullptr);

    UnmapViewOfFile(ctl);
    UnmapViewOfFile(ring);
    second.reset();
    link.reset();
    // Everything released: nothing left to open.
    CHECK(HookLink::open_existing(kFakePid, &error) == nullptr);
}

TEST_CASE("graphics modules: classification and names") {
    CHECK(classify_graphics_module("d3d11.dll") == proto::kApiD3D11);
    CHECK(classify_graphics_module("D3D11.DLL") == proto::kApiD3D11);
    CHECK(classify_graphics_module("d3d9.dll") == proto::kApiD3D9);
    CHECK(classify_graphics_module("d3d10core.dll") == proto::kApiD3D10);
    CHECK(classify_graphics_module("d3d10_1.dll") == proto::kApiD3D10);
    CHECK(classify_graphics_module("d3d12.dll") == proto::kApiD3D12);
    CHECK(classify_graphics_module("opengl32.dll") == proto::kApiOpenGL);
    CHECK(classify_graphics_module("vulkan-1.dll") == proto::kApiVulkan);
    CHECK(classify_graphics_module("dxgi.dll") == proto::kApiDXGI);
    CHECK(classify_graphics_module("d3d11_extra.dll") == 0);
    CHECK(classify_graphics_module("kernel32.dll") == 0);
    CHECK(api_names(0) == "-");
    CHECK(api_names(proto::kApiD3D11 | proto::kApiDXGI) == "D3D11 DXGI");
    CHECK(api_names(proto::kApiOpenGL) == "OpenGL");
    CHECK(is_hook_module("rec_hook64.dll"));
    CHECK(is_hook_module("REC_HOOK32.DLL"));
    CHECK(!is_hook_module("rec_hook.dll"));
}

TEST_CASE("processes: this process is found by name, rec's own listing is sane") {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring name = path;
    name = name.substr(name.find_last_of(L'\\') + 1);
    const std::vector<uint32_t> pids = find_processes_by_name(to_utf8(name));
    CHECK(std::find(pids.begin(), pids.end(), GetCurrentProcessId()) != pids.end());
    ProcessInfo info;
    REQUIRE(process_info(GetCurrentProcessId(), &info));
    CHECK(info.is_64bit);
    CHECK(!info.hook_loaded);
    CHECK(!process_info(0x7FFFFFF0, &info));
}

TEST_CASE("anti-cheat: matching rules") {
    const std::vector<std::string> user = {"EasyAntiCheat", "BEService", "BEDaisy", "vgc", "vgk"};
    // Real components.
    CHECK(matches_anticheat("EasyAntiCheat.exe", user));
    CHECK(matches_anticheat("EasyAntiCheat_EOS.dll", user));
    CHECK(matches_anticheat("easyanticheat_x64.dll", user));
    CHECK(matches_anticheat("BEService_x64.exe", user));
    CHECK(matches_anticheat("BEDaisy.sys", user));
    CHECK(matches_anticheat("vgc.exe", user));
    CHECK(matches_anticheat("vgk.sys", user));
    CHECK(matches_anticheat("Game_EasyAntiCheat", user));  // after a separator
    // Ordinary names must not be caught.
    CHECK(!matches_anticheat("WindscribeService.exe", user));  // contains "beservice" inside a word
    CHECK(!matches_anticheat("vgcore.dll", user));             // short entries match exactly
    CHECK(!matches_anticheat("svgc.exe", user));
    CHECK(!matches_anticheat("rec_testapp.exe", user));
    CHECK(!matches_anticheat("d3d11.dll", user));
    CHECK(!matches_anticheat("", user));
    // The user's own entries count, extension and case ignored.
    CHECK(matches_anticheat("MyGuard.dll", {"myguard.sys"}));
    // The built-in list covers the well-known names without any user entries.
    CHECK(matches_anticheat("EasyAntiCheat_x64.dll", builtin_anticheat_names()));
    CHECK(matches_anticheat("BEClient_x64.dll", builtin_anticheat_names()));
    CHECK(!matches_anticheat("WindscribeService.exe", builtin_anticheat_names()));
}

TEST_CASE("anti-cheat: scanning this process finds nothing") {
    // No test machine is expected to have rec's test binary mistaken for an anti-cheat component.
    const auto modules = scan_anticheat_modules({}, GetCurrentProcessId());
    CHECK(modules.empty());
}
