// rec verify / convert (recorder plan §12.1): the commands that read recordings.
#pragma once

#include <string>

namespace rec_cli {

int cmd_verify(const std::string& file, bool testapp, const std::string& source);
int cmd_convert(const std::string& file, const std::string& container, int crf, const std::string& out);

}  // namespace rec_cli
