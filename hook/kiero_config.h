// Force-included into every kiero2 source (third_party/CMakeLists.txt) and included first by
// kiero_wrap.hpp: routes kiero2's debug output and assertions into the hook's shared-memory log ring
// (recorder plan §4.6.1). The hook must never touch the console, and an assertion must never abort
// the game.
#pragma once

namespace rec::hook {
void kiero_debug(const char* fmt, ...);
void kiero_assert_failed(const char* expression, int line);
}  // namespace rec::hook

#define KIERO_DBG_MSG(...) ::rec::hook::kiero_debug(__VA_ARGS__)
#define KIERO_ASSERT(x) ((void)((x) || (::rec::hook::kiero_assert_failed(#x, __LINE__), 0)))
