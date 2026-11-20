// The setup's command line.
#pragma once
#include <string>
#include <vector>

namespace cli {

struct Args {
    std::vector<std::string> list;
    // "--name value" or "--name=value"
    std::string Get(const std::string& name) const;
    bool Has(const std::string& name) const;
};
Args Parse();
// --prepare: the whole install without a window. Returns the exit code.
int Prepare(const Args& args);

} // namespace cli
