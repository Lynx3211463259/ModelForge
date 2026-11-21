#pragma once

#include "modelforge/settings.hpp"
#include "modelforge/types.hpp"

#include <mutex>

namespace mf {

class ConverterService {
public:
    explicit ConverterService(SettingsService& settings);

    EngineCapabilities capabilities(bool refresh = false);
    json compatibility(const ModelScanResult& scan);
    json estimates(const ModelScanResult& scan);
    json recommend(const ModelScanResult& scan, const std::string& mode,
                   uint64_t availableMemory = 0, uint64_t availableVram = 0);
    json commandPreview(const ConversionParams& params);
    bool validateParams(ConversionParams& params, std::string& code, std::string& error);
    fs::path intermediatePath(const ConversionParams& params) const;
    fs::path finalPath(const ConversionParams& params) const;

    static double bitsPerWeight(const std::string& type);
    static std::string typeDescription(const std::string& type);

private:
    SettingsService& settings_;
    std::mutex cacheMutex_;
    EngineCapabilities cached_;
    int64_t cachedAt_ = 0;
    EngineCapabilities detect();
    json estimateList(const ModelScanResult& scan);
};

} // namespace mf
