// Finding games: which processes have graphics APIs loaded (`rec list`, recorder plan §4.3, §12.1)
// and small helpers around processes.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rec {

struct ProcessInfo {
    uint32_t pid = 0;
    std::string name;       // "javaw.exe"
    bool is_64bit = true;
    uint32_t apis = 0;      // proto::Api bits from the loaded modules
    bool hook_loaded = false;  // rec's hook DLL is already inside
};

// The proto::Api bit of a graphics module ("d3d11.dll" -> kApiD3D11), 0 for any other module.
// Case-insensitive; d3d10.dll, d3d10_1.dll and d3d10core.dll are all D3D10.
uint32_t classify_graphics_module(std::string_view base_name);

// "D3D11 DXGI", in a fixed order; "-" for none.
std::string api_names(uint32_t apis);

// Names of the hook DLLs ("rec_hook64.dll" / "rec_hook32.dll").
bool is_hook_module(std::string_view base_name);

// Every process with at least one of D3D9/10/11/12, OpenGL or Vulkan loaded (DXGI alone is not
// enough: many programs load it), sorted by name. Processes that can't be inspected are skipped.
std::vector<ProcessInfo> list_graphics_processes();

// Basic facts about one process, even without graphics modules; false if it doesn't exist.
bool process_info(uint32_t pid, ProcessInfo* out);

// All processes whose executable name equals `name` (case-insensitive).
std::vector<uint32_t> find_processes_by_name(std::string_view name);

// Processes that have rec's hook DLL loaded.
std::vector<uint32_t> find_hooked_processes();

}  // namespace rec
