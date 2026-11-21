#pragma once

#include "ghc_filesystem.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace mf {

std::string sha1(const std::string& data);
std::string sha1Raw(const std::string& data);
std::string base64Encode(const std::string& data);
std::string base64Decode(const std::string& data);
std::string sha256File(const ghc::filesystem::path& path,
                       const std::function<void(uint64_t, uint64_t)>& progress = {});

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t length);
    std::string hexDigest();

private:
    void transform(const uint8_t* chunk);
    uint32_t state_[8];
    uint64_t bitLength_;
    uint8_t buffer_[64];
    size_t bufferLength_;
};

} // namespace mf
