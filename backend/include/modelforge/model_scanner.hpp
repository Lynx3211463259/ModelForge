#pragma once

#include "modelforge/types.hpp"
#include "modelforge/util.hpp"

#include <string>

namespace mf {

class ModelScanner {
public:
    static ModelScanResult scan(const fs::path& path);
    static json buildInspectionReport(const ModelScanResult& scan);
};

} // namespace mf
