// The rate controller (recorder plan §9): disk levels with hysteresis, the strict mode, CPU overload.
#include <vector>

#include "rec/rate.h"
#include "testfw.h"

using namespace rec;

namespace {

struct Log {
    std::vector<std::pair<int, int>> changes;
    int cpu_started = 0, cpu_ended = 0;
};

void on_level(void* user, int from, int to, const RateInputs&) { static_cast<Log*>(user)->changes.emplace_back(from, to); }
void on_cpu_start(void* user, const RateInputs&) { ++static_cast<Log*>(user)->cpu_started; }
void on_cpu_end(void* user, const RateInputs&) { ++static_cast<Log*>(user)->cpu_ended; }

RateInputs at(int64_t ms, int queue, int ring = 0, double encode = 3.0) {
    RateInputs in;
    in.now_ms = ms;
    in.queue_pct = queue;
    in.ring_pct = ring;
    in.encode_avg_ms = encode;
    return in;
}

void attach(RateController* r, Log* log) {
    r->hooks.level_changed = on_level;
    r->hooks.cpu_overload_started = on_cpu_start;
    r->hooks.cpu_overload_ended = on_cpu_end;
    r->hooks.user = log;
}

}  // namespace

TEST_CASE("rate: an empty queue stays lossless") {
    RateController r;
    for (int i = 0; i < 100; ++i) {
        const RateDecision d = r.decide(at(i * 16, 5));
        CHECK(d.near_level == 0 && !d.drop);
    }
    CHECK(r.level() == 0);
}

TEST_CASE("rate: the queue climbs through the levels at once, with the plan's thresholds") {
    RateController r;
    Log log;
    attach(&r, &log);
    int64_t t = 0;
    CHECK(r.decide(at(t += 16, 39)).near_level == 0);
    CHECK(r.decide(at(t += 16, 40)).near_level == 1);
    CHECK(r.decide(at(t += 16, 60)).near_level == 2);
    CHECK(r.decide(at(t += 16, 75)).near_level == 3);
    RateDecision d = r.decide(at(t += 16, 90));
    CHECK(d.drop && d.near_level == 3);  // level 4: frames become DUPs
    CHECK(r.level() == 4);
    REQUIRE(log.changes.size() == 4);
    CHECK(log.changes[0] == std::make_pair(0, 1));
    CHECK(log.changes[3] == std::make_pair(3, 4));
    // A jump straight to the top is one change.
    RateController r2;
    Log log2;
    attach(&r2, &log2);
    r2.decide(at(0, 0));
    CHECK(r2.decide(at(16, 95)).drop);
    CHECK(log2.changes.size() == 1 && log2.changes[0] == std::make_pair(0, 4));
}

TEST_CASE("rate: stepping down needs the queue below the level's threshold for 2 s, one level at a time") {
    RateController r;
    Log log;
    attach(&r, &log);
    int64_t t = 0;
    r.decide(at(t, 65));  // level 2
    CHECK(r.level() == 2);
    for (; t < 1900; t += 50) r.decide(at(t, 30));
    CHECK(r.level() == 2);  // 1.9 s is not enough
    r.decide(at(t = 2100, 30));
    CHECK(r.level() == 1);
    // A bump above the threshold restarts the clock.
    r.decide(at(t += 1000, 45));
    CHECK(r.level() == 1);
    r.decide(at(t += 1000, 30));
    r.decide(at(t += 1500, 30));
    CHECK(r.level() == 1);
    r.decide(at(t += 600, 30));
    CHECK(r.level() == 0);
}

TEST_CASE("rate: level 4 drops frames until the queue is under 75%, then steps down normally") {
    RateController r;
    int64_t t = 0;
    CHECK(r.decide(at(t, 95)).drop);
    CHECK(r.decide(at(t += 16, 80)).drop);
    CHECK(r.decide(at(t += 16, 76)).drop);
    RateDecision d = r.decide(at(t += 16, 70));
    CHECK(!d.drop && d.near_level == 3);
    CHECK(r.level() == 3);
}

TEST_CASE("rate: rcv-strict is lossless or it drops, never near-lossless") {
    RateSettings s;
    s.strict = true;
    RateController r(s);
    int64_t t = 0;
    CHECK(r.decide(at(t, 50)).near_level == 0);
    CHECK(r.decide(at(t += 16, 80)).near_level == 0);
    CHECK(!r.decide(at(t += 16, 85)).drop);
    CHECK(r.decide(at(t += 16, 92)).drop);
    CHECK(r.decide(at(t += 16, 80)).drop);
    const RateDecision d = r.decide(at(t += 16, 70));
    CHECK(!d.drop && d.near_level == 0);
}

TEST_CASE("rate: a filling frame ring is a CPU overload: drop (never near-lossless) until the ring is under 25%") {
    RateController r;
    Log log;
    attach(&r, &log);
    int64_t t = 0;
    CHECK(!r.decide(at(t, 0, 49)).drop);
    RateDecision d = r.decide(at(t += 16, 0, 50));
    CHECK(d.drop && d.near_level == 0);  // the disk is fine: stays lossless
    CHECK(r.cpu_overloaded());
    CHECK(r.decide(at(t += 16, 0, 40)).drop);
    CHECK(r.decide(at(t += 16, 0, 25)).drop);
    CHECK(!r.decide(at(t += 16, 0, 24)).drop);
    CHECK(log.cpu_started == 1 && log.cpu_ended == 1);
}

TEST_CASE("rate: slow encoding with a ring that is not empty is an overload, slow encoding with an empty ring is not") {
    RateController r;
    int64_t t = 0;
    CHECK(!r.decide(at(t, 0, 5, 14.0)).drop);      // 14 ms of a 16.7 ms frame, ring empty enough: keeping up
    CHECK(!r.decide(at(t += 16, 0, 24, 14.0)).drop);
    CHECK(r.decide(at(t += 16, 0, 26, 14.0)).drop);  // over 80% of the frame and the ring is filling
    CHECK(!r.decide(at(t += 16, 0, 10, 14.0)).drop);
}

TEST_CASE("rate: time at each level is accounted") {
    RateController r;
    r.decide(at(0, 0));
    r.decide(at(1000, 0));
    r.decide(at(1000, 65));  // level 2 from 1000 ms
    r.decide(at(3500, 65));
    const auto t = r.ms_at_level(4000);
    CHECK(std::abs(t[0] - 1000) < 1);
    CHECK(std::abs(t[2] - 3000) < 1);
    CHECK(t[1] == 0 && t[4] == 0);
}
