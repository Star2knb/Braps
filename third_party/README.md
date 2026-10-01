# Third-party code (recorder only)

Vendored copies at pinned versions (recorder plan §2, §4.6.1, §18). The codec uses none of these.
Only the files needed to build are kept (headers, sources, licences); tests, docs and examples are
left out. Do not edit these files: wrap or configure them from our own code instead, so an update is
a plain copy of a newer version.

| Library | Version | Commit | Licence | Source | Kept | Used for |
|---|---|---|---|---|---|---|
| CLI11 | v2.7.2 | `cbd58a3` | BSD-3-Clause | https://github.com/CLIUtils/CLI11 | `include/`, `LICENSE` | `rec` command line (header-only) |
| toml++ | v3.4.0 | `3017243` | MIT | https://github.com/marzer/tomlplusplus | `toml.hpp` (single header), `LICENSE` | `rec.toml` |
| spdlog | v1.17.0 | `79524dd` | MIT (bundled {fmt}: MIT) | https://github.com/gabime/spdlog | `include/`, `src/`, `LICENSE` | host logging (async logger) |
| MinHook | v1.3.4 | `c3fcafd` | BSD-2-Clause | https://github.com/TsudaKageyu/minhook | `include/`, `src/`, `LICENSE.txt`, `AUTHORS.txt` | inline hooks in the hook DLLs (x86 + x64) |
| kiero2 | master | `8f57dd9` (= reviewed commit, plan §4.6.1) | MIT | https://github.com/kirchesz/kiero2 | all files except `.gitignore` | finding D3D9/11/12 vtable addresses at install time |

Downloaded 2026-10-01 (shallow clones of the release tags; kiero2 has no releases, so its commit is
pinned instead).
