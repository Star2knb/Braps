// rec verify / convert (recorder plan §12.1): the commands that read recordings.
#pragma once

#include <string>

#include "rec/config.h"

namespace rec_cli {

int cmd_verify(const std::string& file, bool testapp, const std::string& source);
// rec bench-disk [--path P] [--size S]: writes with the recorder's writer, prints the speed, caches it per volume.
int cmd_bench_disk(const rec::Config& cfg, const std::string& path, const std::string& size);
int cmd_convert(const std::string& file, const std::string& container, int crf, const std::string& out);

}  // namespace rec_cli
