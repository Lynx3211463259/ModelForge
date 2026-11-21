#pragma once

#include "modelforge/util.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mf {

struct HfFileEntry {
    std::string path;
    uint64_t size = 0;
    std::string sha256;      // only when Hugging Face exposes a 64-hex LFS digest
    std::string category;    // safetensors | pytorch | gguf | config | tokenizer | vocab | readme | index | other
    bool lfs = false;
};

struct HfRepository {
    std::string repoId;
    std::string author;
    std::string name;
    std::string revision = "main";
    std::string commitHash;
    std::string repoType = "model";
    std::string license;
    std::string pipelineTag;
    std::string library;
    std::string description;
    uint64_t totalSize = 0;
    int fileCount = 0;
    bool gated = false;
    bool privateRepo = false;
    std::vector<HfFileEntry> files;
    json toJson() const;
};

struct HfTokenInfo {
    bool valid = false;
    std::string username;
    std::string organization;
    std::string code;
    std::string error;
};

struct HfResolveResult {
    bool ok = false;
    HfRepository repository;
    std::string code;
    std::string error;
    std::string suggestion;
    json details = nullptr;
};

class HfClient {
public:
    // Accepts "owner/name", full page URLs, /tree/<rev> URLs and "owner/name@revision".
    static bool parseRepositoryInput(const std::string& input, const std::string& explicitRevision,
                                     std::string& repoId, std::string& revision, std::string& error);
    static HfResolveResult resolve(const std::string& input, const std::string& revision, const std::string& token,
                                   int timeoutMs = 20000);
    static HfTokenInfo validateToken(const std::string& token, int timeoutMs = 15000);
    static std::string resolveUrl(const std::string& repoId, const std::string& revision, const std::string& path);
    static std::string apiModelUrl(const std::string& repoId, const std::string& revision);
    static std::string apiTreeUrl(const std::string& repoId, const std::string& revision);
    static std::string classifyFile(const std::string& path);
    static bool isWeightFile(const std::string& path);
    static bool isConfigFile(const std::string& path);
    static std::string urlEncodePath(const std::string& path);
    // Rejects absolute paths, traversal, Windows reserved names and control characters.
    static bool isSafeRelativePath(const std::string& remotePath, std::string& reason);
    static fs::path localPathFor(const fs::path& destination, const std::string& remotePath, std::string& error);
};

} // namespace mf
