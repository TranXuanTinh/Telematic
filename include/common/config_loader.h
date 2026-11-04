#pragma once

#include "common/types.h"
#include <string>

namespace tcu::common {

class ConfigLoader {
public:
    /// Load TCU configuration from a YAML file (returns defaults if path is empty).
    static TcuConfig load(const std::string& yaml_path);

    /// Parse standard CLI flags (--config <path>, --mock, --runtime-dir <dir>).
    static TcuConfig loadFromArgs(int argc, char* argv[], bool* force_mock_out = nullptr);
};

} // namespace tcu::common

