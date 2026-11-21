#pragma once

#include "modelforge/util.hpp"

namespace mf {

struct GgufInfo {
    bool valid = false;
    uint32_t version = 0;
    uint64_t tensorCount = 0;
    uint64_t metadataCount = 0;
    uint64_t fileSize = 0;
    std::string architecture;
    std::string modelName;
    std::string quantType;
    json metadata = json::object();
    json tensors = json::array();
    std::vector<std::string> errors;
    json toJson() const;
};

class GgufReader {
public:
    static GgufInfo read(const fs::path& path, bool includeTensorNames = true);
    static std::string quantName(int fileType);
};

} // namespace mf
