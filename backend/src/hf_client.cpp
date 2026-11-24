#include "modelforge/hf_client.hpp"

#include "modelforge/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace mf {

namespace {

const char* kHfHost = "huggingface.co";

bool isValidRepoId(const std::string& value) {
    if (value.empty() || value.size() > 200) return false;
    const size_t slash = value.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= value.size()) return false;
    if (value.find('/', slash + 1) != std::string::npos) return false;
    for (char c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '/') continue;
        return false;
    }
    return true;
}

std::string stripQueryAndFragment(std::string value) {
    const size_t cut = value.find_first_of("?#");
    if (cut != std::string::npos) value = value.substr(0, cut);
    while (!value.empty() && value.back() == '/') value.pop_back();
    if (endsWith(value, ".git")) value = value.substr(0, value.size() - 4);
    return value;
}

std::string headerToken(const std::string& token) {
    if (token.empty()) return std::string();
    return "Bearer " + token;
}

std::string licenseFromTags(const json& tags) {
    if (!tags.is_array()) return std::string();
    for (const auto& tag : tags) {
        if (!tag.is_string()) continue;
        const std::string value = tag.get<std::string>();
        if (startsWith(value, "license:")) return value.substr(8);
    }
    return std::string();
}

std::string textField(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) return std::string();
    const auto& value = object[key];
    if (value.is_string()) return value.get<std::string>();
    return std::string();
}

void addFile(std::vector<HfFileEntry>& files, std::set<std::string>& seen, HfFileEntry entry,
             const std::string& repoType) {
    if (entry.path.empty() || seen.count(entry.path)) return;
    entry.category = HfClient::classifyFile(entry.path, repoType);
    seen.insert(entry.path);
    files.push_back(std::move(entry));
}

HfResolveResult makeError(const std::string& code, const std::string& message, const std::string& suggestion) {
    HfResolveResult result;
    result.code = code;
    result.error = message;
    result.suggestion = suggestion;
    return result;
}

} // namespace

json HfRepository::toJson() const {
    json list = json::array();
    for (const auto& file : files) {
        list.push_back(json{{"path", file.path}, {"size", file.size}, {"category", file.category},
                            {"sha256", file.sha256}, {"lfs", file.lfs}});
    }
    return json{
        {"repoId", repoId}, {"author", author}, {"name", name}, {"revision", revision},
        {"commitHash", commitHash}, {"repoType", repoType}, {"license", license},
        {"pipelineTag", pipelineTag}, {"library", library}, {"description", description},
        {"totalSize", totalSize}, {"fileCount", fileCount}, {"gated", gated},
        {"private", privateRepo}, {"files", list}
    };
}

bool HfClient::parseRepositoryInput(const std::string& input, const std::string& explicitRevision,
                                   std::string& repoId, std::string& revision, std::string& error) {
    std::string ignoredType;
    return parseRepositoryInput(input, explicitRevision, repoId, revision, error, ignoredType);
}

bool HfClient::parseRepositoryInput(const std::string& input, const std::string& explicitRevision,
                                   std::string& repoId, std::string& revision, std::string& error,
                                   std::string& repoType) {
    repoId.clear();
    repoType = "model";
    revision = explicitRevision.empty() ? "main" : trim(explicitRevision);
    std::string value = trim(input);
    if (value.empty()) {
        error = "请输入 Hugging Face 仓库地址或仓库 ID";
        return false;
    }

    // owner/name@revision
    const size_t at = value.rfind('@');
    if (at != std::string::npos && value.find("://") == std::string::npos) {
        if (revision.empty() || revision == "main") revision = value.substr(at + 1);
        value = value.substr(0, at);
    }

    const size_t scheme = value.find("://");
    if (scheme != std::string::npos) value = value.substr(scheme + 3);
    if (startsWith(toLower(value), "www.huggingface.co")) value = value.substr(std::string("www.huggingface.co").size());
    else if (startsWith(toLower(value), kHfHost)) value = value.substr(std::string(kHfHost).size());
    else if (startsWith(toLower(value), "hf.co")) value = value.substr(std::string("hf.co").size());
    if (!value.empty() && (value[0] == '/' || value[0] == '\\')) value = value.substr(1);

    value = stripQueryAndFragment(value);
    if (value.empty()) {
        error = "无法从输入中提取仓库 ID";
        return false;
    }
    std::vector<std::string> segments;
    {
        std::string current;
        for (char c : value) {
            if (c == '/') {
                if (!current.empty()) segments.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) segments.push_back(current);
    }
    // https://huggingface.co/datasets/<owner>/<name>, /spaces/, /models/ prefixes.
    if (!segments.empty()) {
        const std::string head = toLower(segments[0]);
        if ((head == "datasets" || head == "spaces" || head == "models") && segments.size() >= 3) {
            repoType = head == "datasets" ? "dataset" : "model";
            segments.erase(segments.begin());
        }
    }
    if (segments.size() < 2) {
        error = "仓库 ID 需要 owner/name 形式，例如 Qwen/Qwen3-0.6B";
        return false;
    }
    repoId = segments[0] + "/" + segments[1];
    // https://huggingface.co/<owner>/<name>/tree/<rev>/...
    if (segments.size() >= 4 && (segments[2] == "tree" || segments[2] == "blob")) {
        if (explicitRevision.empty()) revision = segments[3];
    }
    if (!isValidRepoId(repoId)) {
        error = "仓库 ID 包含非法字符: " + repoId;
        return false;
    }
    if (revision.empty()) revision = "main";
    for (char c : revision) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            error = "分支或 Commit 不能包含空白字符";
            return false;
        }
    }
    return true;
}

std::string HfClient::urlEncodePath(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

HfTokenInfo HfClient::validateToken(const std::string& token, int timeoutMs) {
    HfTokenInfo info;
    if (trim(token).empty()) {
        info.code = "TOKEN_EMPTY";
        info.error = "Token 为空";
        return info;
    }
    HttpRequestOptions options;
    options.connectTimeoutMs = timeoutMs;
    options.receiveTimeoutMs = timeoutMs;
    options.headers.push_back({"Authorization", "Bearer " + trim(token)});
    options.headers.push_back({"Accept", "application/json"});
    const HttpResponse response = HttpClient::request("https://huggingface.co/api/whoami-v2", options);
    if (response.status == 200) {
        try {
            const json body = json::parse(response.body);
            info.valid = true;
            info.username = textField(body, "name");
            if (body.contains("orgs") && body["orgs"].is_array() && !body["orgs"].empty()) {
                info.organization = textField(body["orgs"][0], "name");
            }
        } catch (...) {
            info.valid = true;
        }
        return info;
    }
    if (response.status == 401 || response.status == 403) {
        info.code = "TOKEN_INVALID";
        info.error = "Hugging Face 拒绝了这个 Token（HTTP " + std::to_string(response.status) + "）";
        return info;
    }
    info.code = "TOKEN_CHECK_FAILED";
    info.error = response.status == 0 ? ("无法连接 Hugging Face: " + HttpClient::redact(response.error))
                                      : ("Token 校验失败（HTTP " + std::to_string(response.status) + "）");
    return info;
}

std::string HfClient::normalizeRepoType(const std::string& value) {
    const std::string lower = toLower(trim(value));
    if (lower == "dataset" || lower == "datasets" || lower == "data") return "dataset";
    return "model";
}

std::string HfClient::apiModelUrl(const std::string& repoId, const std::string& revision,
                                  const std::string& repoType) {
    const std::string prefix = normalizeRepoType(repoType) == "dataset" ? "/api/datasets/" : "/api/models/";
    return "https://huggingface.co" + prefix + urlEncodePath(repoId) +
           "?revision=" + urlEncodePath(revision) + "&blobs=true";
}

std::string HfClient::apiTreeUrl(const std::string& repoId, const std::string& revision,
                                 const std::string& repoType) {
    const std::string prefix = normalizeRepoType(repoType) == "dataset" ? "/api/datasets/" : "/api/models/";
    return "https://huggingface.co" + prefix + urlEncodePath(repoId) + "/tree/" +
           urlEncodePath(revision) + "?recursive=true&expand=true";
}

std::string HfClient::resolveUrl(const std::string& repoId, const std::string& revision, const std::string& path,
                                 const std::string& repoType) {
    const std::string prefix = normalizeRepoType(repoType) == "dataset" ? "/datasets/" : "/";
    return "https://huggingface.co" + prefix + urlEncodePath(repoId) + "/resolve/" + urlEncodePath(revision) + "/" +
           urlEncodePath(path);
}

std::string HfClient::classifyFile(const std::string& path, const std::string& repoType) {
    if (normalizeRepoType(repoType) == "dataset") return classifyDatasetFile(path);
    const std::string lower = toLower(path);
    const std::string name = lower.substr(lower.find_last_of('/') == std::string::npos ? 0 : lower.find_last_of('/') + 1);
    if (endsWith(name, ".safetensors")) return "safetensors";
    if (endsWith(name, ".bin") || endsWith(name, ".pt") || endsWith(name, ".pth") || endsWith(name, ".ckpt")) return "pytorch";
    if (endsWith(name, ".gguf")) return "gguf";
    if (endsWith(name, "tokenizer.model") || endsWith(name, ".spm") || endsWith(name, "vocab.txt") ||
        endsWith(name, "vocab.json") || endsWith(name, "merges.txt")) return "vocab";
    if (name.find("tokenizer") != std::string::npos || endsWith(name, "special_tokens_map.json") ||
        endsWith(name, "added_tokens.json")) return "tokenizer";
    if (name == "config.json" || name == "generation_config.json" || endsWith(name, "config.json")) return "config";
    if (name.find("index.json") != std::string::npos) return "index";
    if (name == "readme.md" || endsWith(name, ".md")) return "readme";
    return "other";
}

std::string HfClient::classifyDatasetFile(const std::string& path) {
    const std::string lower = toLower(path);
    const size_t slash = lower.find_last_of('/');
    const std::string name = slash == std::string::npos ? lower : lower.substr(slash + 1);
    if (name == "readme.md" || endsWith(name, ".md")) return "readme";
    if (endsWith(name, ".parquet")) return "parquet";
    if (endsWith(name, ".arrow") || endsWith(name, ".arrow.gz")) return "arrow";
    if (endsWith(name, ".csv") || endsWith(name, ".tsv")) return "csv";
    if (endsWith(name, ".jsonl") || endsWith(name, ".jsonlines") || endsWith(name, ".ndjson")) return "jsonl";
    if (endsWith(name, ".json")) return "json";
    if (endsWith(name, ".txt") || endsWith(name, ".text") || endsWith(name, ".raw")) return "text";
    if (endsWith(name, ".png") || endsWith(name, ".jpg") || endsWith(name, ".jpeg") || endsWith(name, ".webp") ||
        endsWith(name, ".bmp") || endsWith(name, ".gif")) return "image";
    if (endsWith(name, ".wav") || endsWith(name, ".mp3") || endsWith(name, ".flac") || endsWith(name, ".ogg") ||
        endsWith(name, ".m4a")) return "audio";
    if (endsWith(name, ".mp4") || endsWith(name, ".mkv") || endsWith(name, ".webm") || endsWith(name, ".mov")) return "video";
    if (endsWith(name, ".zip") || endsWith(name, ".tar") || endsWith(name, ".gz") || endsWith(name, ".tgz") ||
        endsWith(name, ".7z") || endsWith(name, ".bz2") || endsWith(name, ".xz")) return "archive";
    if (endsWith(name, ".safetensors") || endsWith(name, ".bin") || endsWith(name, ".pt") || endsWith(name, ".pth") ||
        endsWith(name, ".gguf") || endsWith(name, ".onnx") || endsWith(name, ".npy") || endsWith(name, ".npz") ||
        endsWith(name, ".pkl") || endsWith(name, ".h5")) return "tensor";
    if (name.find("index.json") != std::string::npos) return "index";
    if (endsWith(name, ".py") || endsWith(name, ".ipynb") || endsWith(name, ".yaml") || endsWith(name, ".yml") ||
        endsWith(name, ".toml") || endsWith(name, ".cfg")) return "config";
    return "other";
}

bool HfClient::isDatasetDataFile(const std::string& path) {
    const std::string category = classifyDatasetFile(path);
    return category == "parquet" || category == "arrow" || category == "csv" || category == "jsonl" ||
           category == "json" || category == "text" || category == "image" || category == "audio" ||
           category == "video" || category == "archive" || category == "tensor";
}

bool HfClient::isWeightFile(const std::string& path) {
    const std::string category = classifyFile(path);
    return category == "safetensors" || category == "pytorch" || category == "gguf";
}

bool HfClient::isConfigFile(const std::string& path) {
    const std::string category = classifyFile(path);
    return category == "config" || category == "tokenizer" || category == "vocab" || category == "index";
}

bool HfClient::isSafeRelativePath(const std::string& remotePath, std::string& reason) {
    if (remotePath.empty()) { reason = "空文件名"; return false; }
    if (remotePath.size() > 1024) { reason = "路径过长"; return false; }
    if (remotePath[0] == '/' || remotePath[0] == '\\') { reason = "绝对路径不允许"; return false; }
    if (remotePath.find('\\') != std::string::npos) { reason = "路径中包含反斜杠"; return false; }
    if (remotePath.find(':') != std::string::npos) { reason = "路径中包含冒号"; return false; }
    for (unsigned char c : remotePath) {
        if (c < 0x20 || c == 0x7F) { reason = "路径中包含控制字符"; return false; }
    }
    std::vector<std::string> parts = split(remotePath, '/');
    for (const auto& part : parts) {
        if (part.empty() || part == "." || part == "..") { reason = "路径中包含空段或上级跳转"; return false; }
        const std::string lower = toLower(part);
        static const std::set<std::string> reserved = {
            "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
            "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
        };
        std::string stem = lower;
        const size_t dot = stem.find('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        if (reserved.count(stem)) { reason = "Windows 保留文件名: " + part; return false; }
        if (!part.empty() && (part.back() == ' ' || part.back() == '.')) { reason = "文件名不能以空格或点结尾"; return false; }
    }
    return true;
}

fs::path HfClient::localPathFor(const fs::path& destination, const std::string& remotePath, std::string& error) {
    std::string reason;
    if (!isSafeRelativePath(remotePath, reason)) {
        error = "拒绝不安全的仓库路径 '" + remotePath + "': " + reason;
        return {};
    }
    fs::path relative;
    for (const auto& part : split(remotePath, '/')) relative /= pathFromUtf8(part);
    const fs::path base = fs::absolute(destination);
    const fs::path target = base / relative;
    if (!isPathInside(base, target)) {
        error = "目标路径逃逸出下载目录: " + remotePath;
        return {};
    }
    return target;
}

HfResolveResult HfClient::resolve(const std::string& input, const std::string& revisionInput, const std::string& token,
                                  int timeoutMs, const std::string& requestedRepoType) {
    std::string repoId, revision, parseError, detectedType;
    if (!parseRepositoryInput(input, revisionInput, repoId, revision, parseError, detectedType)) {
        return makeError("INVALID_REPOSITORY", parseError, "使用 owner/name 或完整 Hugging Face 仓库地址。");
    }
    const std::string repoType = trim(requestedRepoType).empty() ? detectedType : normalizeRepoType(requestedRepoType);
    const bool isDataset = repoType == "dataset";

    HttpRequestOptions options;
    options.connectTimeoutMs = timeoutMs;
    options.receiveTimeoutMs = timeoutMs;
    if (!token.empty()) options.headers.push_back({"Authorization", headerToken(token)});
    options.headers.push_back({"Accept", "application/json"});

    HttpResponse response = HttpClient::request(apiModelUrl(repoId, revision, repoType), options);
    if (response.status == 0) {
        return makeError("HF_NETWORK_ERROR", "无法访问 Hugging Face: " + HttpClient::redact(response.error),
                         "检查网络、代理与 DNS 设置，或稍后重试。");
    }
    if (response.status == 401) {
        return makeError("HF_TOKEN_REQUIRED",
                         "仓库不存在、属于私有仓库，或当前 Token 无效（HTTP 401）。Hugging Face 对无权限仓库统一返回 401。",
                         "检查仓库 ID 与 revision；私有仓库请在下载中心配置具备 read 权限的 Token。");
    }
    if (response.status == 403) {
        return makeError("HF_FORBIDDEN", "没有访问权限，或该仓库需要先在网页上接受使用协议（HTTP 403）。",
                         "先在 huggingface.co 上接受" + std::string(isDataset ? "数据集" : "模型") +
                             "许可协议，再使用具备 read 权限的 Token。");
    }
    if (response.status == 404) {
        return makeError("HF_REPOSITORY_NOT_FOUND",
                         std::string(isDataset ? "数据集" : "仓库") + "不存在或分支/Commit 无效: " + repoId + "@" + revision,
                         "检查仓库 ID 与 revision 拼写" + std::string(isDataset ? "，并确认这是 datasets 而非 models 路径。" : "。"));
    }
    if (response.status == 429) {
        return makeError("HF_RATE_LIMITED", "Hugging Face 接口限流（HTTP 429）。",
                         "稍后重试；配置 Token 可提高限额。");
    }
    if (!response.ok) {
        return makeError("HF_REQUEST_FAILED", "仓库解析失败（HTTP " + std::to_string(response.status) + "）",
                         "稍后重试或检查网络。");
    }

    json model;
    try {
        model = json::parse(response.body);
    } catch (const std::exception& ex) {
        return makeError("HF_INVALID_RESPONSE", std::string("Hugging Face 返回了无法解析的响应: ") + ex.what(),
                         "稍后重试。");
    }
    if (!model.is_object()) {
        return makeError("HF_INVALID_RESPONSE", "Hugging Face 返回的仓库信息格式异常", "稍后重试。");
    }

    HfResolveResult result;
    result.ok = true;
    HfRepository& repo = result.repository;
    repo.repoId = repoId;
    repo.repoType = repoType;
    repo.revision = revision;
    repo.commitHash = textField(model, "sha");
    repo.privateRepo = model.value("private", false);
    repo.author = textField(model, "author");
    if (repo.author.empty()) repo.author = repoId.substr(0, repoId.find('/'));
    repo.name = repoId.substr(repoId.find('/') + 1);
    if (model.contains("gated") && !model["gated"].is_null()) {
        repo.gated = model["gated"].is_boolean() ? model["gated"].get<bool>() : true;
    }
    repo.library = textField(model, "library_name");
    repo.pipelineTag = textField(model, "pipeline_tag");
    if (model.contains("cardData") && model["cardData"].is_object()) {
        repo.license = textField(model["cardData"], "license");
        repo.description = textField(model["cardData"], "model_name");
        if (repo.description.empty()) repo.description = textField(model["cardData"], "language");
    }
    if (repo.license.empty()) repo.license = licenseFromTags(model.value("tags", json::array()));

    std::set<std::string> seen;
    // The recursive tree endpoint gives accurate sizes and LFS digests for every file.
    HttpResponse tree = HttpClient::request(apiTreeUrl(repoId, revision, repoType), options);
    if (tree.ok) {
        try {
            const json entries = json::parse(tree.body);
            if (entries.is_array()) {
                for (const auto& entry : entries) {
                    if (!entry.is_object()) continue;
                    if (entry.value("type", "file") != "file") continue;
                    HfFileEntry file;
                    file.path = textField(entry, "path");
                    if (entry.contains("size") && entry["size"].is_number()) file.size = entry["size"].get<uint64_t>();
                    if (entry.contains("lfs") && entry["lfs"].is_object()) {
                        file.lfs = true;
                        const auto& lfs = entry["lfs"];
                        if (lfs.contains("size") && lfs["size"].is_number()) file.size = lfs["size"].get<uint64_t>();
                        const std::string oid = textField(lfs, "oid");
                        if (oid.size() == 64) file.sha256 = oid;
                    }
                    addFile(repo.files, seen, std::move(file), repoType);
                }
            }
        } catch (...) {
            // Fall through to the sibling list below.
        }
    }
    if (repo.files.empty() && model.contains("siblings") && model["siblings"].is_array()) {
        for (const auto& sibling : model["siblings"]) {
            if (!sibling.is_object()) continue;
            HfFileEntry file;
            file.path = textField(sibling, "rfilename");
            if (sibling.contains("size") && sibling["size"].is_number()) file.size = sibling["size"].get<uint64_t>();
            if (sibling.contains("lfs") && sibling["lfs"].is_object()) {
                file.lfs = true;
                const auto& lfs = sibling["lfs"];
                if (lfs.contains("size") && lfs["size"].is_number()) file.size = lfs["size"].get<uint64_t>();
                const std::string oid = textField(lfs, "oid");
                if (oid.size() == 64) file.sha256 = oid;
            }
            addFile(repo.files, seen, std::move(file), repoType);
        }
    }
    std::sort(repo.files.begin(), repo.files.end(), [](const HfFileEntry& a, const HfFileEntry& b) {
        return a.path < b.path;
    });
    repo.fileCount = static_cast<int>(repo.files.size());
    repo.totalSize = 0;
    int weightFiles = 0;
    int dataFiles = 0;
    for (const auto& file : repo.files) {
        repo.totalSize += file.size;
        if (isWeightFile(file.path)) ++weightFiles;
        if (isDatasetDataFile(file.path)) ++dataFiles;
    }
    if (repo.files.empty()) {
        result.ok = false;
        result.code = "HF_EMPTY_REPOSITORY";
        result.error = "仓库中没有可下载的文件";
        result.suggestion = "确认 revision 是否正确。";
        return result;
    }
    if (isDataset) {
        if (dataFiles == 0) {
            result.details = json{{"warning", "数据集中没有常见的 parquet/arrow/csv/jsonl/压缩包文件，可能只是说明或脚本。"}};
        }
    } else if (weightFiles == 0) {
        result.details = json{{"warning", "仓库中没有 .safetensors/.bin/.gguf 权重文件，可能只是配置或数据集仓库。"}};
    }
    return result;
}

} // namespace mf
