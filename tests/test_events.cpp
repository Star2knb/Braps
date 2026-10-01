// Event-code registry (recorder plan §10.4).
#include <set>
#include <string>

#include "rec/events.h"
#include "testfw.h"

using namespace rec;

TEST_CASE("events: codes and names are unique") {
    std::set<std::string> codes, names;
    for (const EventInfo& e : kEvents) {
        CHECK(codes.insert(e.code).second);
        CHECK(names.insert(e.name).second);
        CHECK(std::string(e.description).size() > 5);
    }
    CHECK(kEventCount >= 47);
}

TEST_CASE("events: level, subsystem and number come from the code") {
    CHECK(event_level(Ev::SlowWrite) == Level::Warn);
    CHECK(event_subsystem(Ev::SlowWrite) == Subsystem::Disk);
    CHECK(event_number(Ev::SlowWrite) == 4101);
    CHECK(std::string(event_info(Ev::SlowWrite).name) == "slow_write");
    CHECK(event_level(Ev::UnhandledException) == Level::Fatal);
    CHECK(event_level(Ev::BackendSelected) == Level::Info);
    CHECK(event_level(Ev::LocateFailed) == Level::Error);
    CHECK(event_subsystem(Ev::HotkeyRegistrationFailed) == Subsystem::Cli);
    // The plan uses 1301 twice; the letter tells them apart.
    CHECK(event_number(Ev::DisplayRefresh) == event_number(Ev::PacingError));
    CHECK(event_level(Ev::DisplayRefresh) != event_level(Ev::PacingError));
    CHECK(std::string(subsystem_name(Subsystem::Disk)) == "writer");
    CHECK(std::string(level_name(Level::Warn)) == "WARN");
}

TEST_CASE("events: lookup by code or name") {
    const EventInfo* e = find_event("W4101");
    REQUIRE(e != nullptr);
    CHECK(e->id == Ev::SlowWrite);
    CHECK(find_event("slow_write") == e);
    CHECK(find_event("W9999") == nullptr);
    CHECK(find_event("") == nullptr);
    for (const EventInfo& x : kEvents) CHECK(find_event(x.code) == &x);
}
