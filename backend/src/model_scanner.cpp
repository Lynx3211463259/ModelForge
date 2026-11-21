#include "modelforge/model_scanner.hpp"

#include "modelforge/util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace mf {

namespace {
uint64_t readU64Le(const unsigned char* data) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | data[i];
    return value;
}

std::string architectureFamily(const std::string& architecture, const std::string& modelType) {
    const std::string value = toLower(architecture + " " + modelType);
    if (value.find("qwen") != std::string::npos) return "Qwen";
    if (value.find("deepseek") != std::string::npos) return "DeepSeek";
    if (value.find("mistral") != std::string::npos || value.find("mixtral") != std::string::npos) return "Mistral";
    if (value.find("gemma") != std::string::npos) return "Gemma";
    if (value.find("phi") != std::string::npos) return "Phi";
    if (value.find("falcon") != std::string::npos) return "Falcon";
    if (value.find("gptneox") != std::string::npos || value.find("gpt_neox") != std::string::npos) return "GPT-NeoX";
    if (value.find("llama") != std::string::npos) return "LLaMA";
    if (value.find("baichuan") != std::string::npos) return "Baichuan";
    if (value.find("glm") != std::string::npos) return "GLM";
    if (value.find("internlm") != std::string::npos) return "InternLM";
    if (value.find("yi") != std::string::npos) return "Yi";
    return architecture.empty() ? "Unknown" : architecture;
}

std::string dominantDtype(const std::map<std::string, uint64_t>& counts) {
    std::string best;
    uint64_t bestCount = 0;
    for (const auto& pair : counts) {
        if (pair.second > bestCount) {
            best = pair.first;
            bestCount = pair.second;
        }
    }
    return best;
}

std::string fileKind(const std::string& name) {
    const std::string lower = toLower(name);
    if (lower == "config.json") return "config";
    if (lower == "tokenizer.json") return "tokenizer";
    if (lower == "tokenizer_config.json") return "tokenizer-config";
    if (lower == "special_tokens_map.json") return "special-tokens";
    if (lower == "generation_config.json") return "generation-config";
    if (lower == "tokenizer.model") return "tokenizer-model";
    if (lower == "vocab.json" || lower == "merges.txt") return "tokenizer-vocab";
    if (endsWith(lower, ".safetensors.index.json")) return "index";
    if (endsWith(lower, ".safetensors")) return "weights";
    if (endsWith(lower, ".part") || endsWith(lower, ".tmp") || endsWith(lower, ".download")) return "incomplete";
    if (endsWith(lower, ".json")) return "metadata";
    return "other";
}

int intFromConfig(const json& config, std::initializer_list<const char*> keys, int fallback = 0) {
    for (const char* key : keys) {
        if (config.contains(key) && config[key].is_number_integer()) return config[key].get<int>();
    }
    return fallback;
}

bool readSafetensorsHeader(const fs::path& path, json& header, std::string& error,
                           uint64_t& tensorParams, int& tensors, std::map<std::string, uint64_t>& dtypeCounts) {
    std::ifstream input(path.u8string(), std::ios::binary);
    if (!input) { error = "无法打开权重文件"; return false; }
    unsigned char lengthBytes[8]{};
    input.read(reinterpret_cast<char*>(lengthBytes), 8);
    if (input.gcount() != 8) { error = "Safetensors 头部长度不完整"; return false; }
    const uint64_t headerLength = readU64Le(lengthBytes);
    if (headerLength < 2 || headerLength > 100ull * 1024 * 1024) {
        error = "Safetensors 头部长度异常: " + std::to_string(headerLength);
        return false;
    }
    std::string raw(static_cast<size_t>(headerLength), '\0');
    input.read(&raw[0], static_cast<std::streamsize>(raw.size()));
    if (static_cast<uint64_t>(input.gcount()) != headerLength) {
        error = "Safetensors 头部被截断";
        return false;
    }
    try {
        header = json::parse(raw);
    } catch (const std::exception& ex) {
        error = "Safetensors 头部 JSON 无效: " + std::string(ex.what());
        return false;
    }
    if (!header.is_object()) { error = "Safetensors 头部根节点不是对象"; return false; }
    for (auto it = header.begin(); it != header.end(); ++it) {
        if (it.key() == "__metadata__") continue;
        if (!it.value().is_object()) continue;
        ++tensors;
        const std::string dtype = it.value().value("dtype", std::string("UNKNOWN"));
        dtypeCounts[dtype]++;
        if (it.value().contains("shape") && it.value()["shape"].is_array()) {
            uint64_t count = 1;
            for (const auto& dim : it.value()["shape"]) {
                if (dim.is_number_unsigned()) count *= dim.get<uint64_t>();
                else if (dim.is_number_integer() && dim.get<int64_t>() > 0) count *= static_cast<uint64_t>(dim.get<int64_t>());
            }
            tensorParams += count;
        }
    }
    return true;
}

json checkItem(const std::string& id, const std::string& label, const std::string& status, const std::string& detail) {
    return json{{"id", id}, {"label", label}, {"status", status}, {"detail", detail}};
}
} // namespace

ModelScanResult ModelScanner::scan(const fs::path& inputPath) {
    ModelScanResult result;
    std::error_code ec;
    fs::path directory = inputPath;
    if (fs::is_regular_file(inputPath, ec)) {
        directory = inputPath.parent_path();
        result.warnings.push_back("已选择单个权重文件，实际扫描其所在目录。GGUF 转换通常需要完整模型目录。");
    }
    if (!fs::exists(directory, ec) || !fs::is_directory(directory, ec)) {
        result.errors.push_back("模型路径不存在或不是目录: " + pathToUtf8(inputPath));
        result.report = buildInspectionReport(result);
        return result;
    }
    directory = fs::weakly_canonical(directory, ec);
    result.path = pathToUtf8(directory);
    result.name = directory.filename().u8string();
    if (result.name.empty()) result.name = "model";

    fs::path configPath = directory / "config.json";
    if (fs::exists(configPath, ec)) {
        try {
            result.config = readJsonFile(configPath);
            result.name = result.config.value("_name_or_path", result.name);
            const auto slash = result.name.find_last_of("/\\");
            if (slash != std::string::npos) result.name = result.name.substr(slash + 1);
            if (result.config.contains("architectures") && result.config["architectures"].is_array() &&
                !result.config["architectures"].empty() && result.config["architectures"][0].is_string()) {
                result.architecture = result.config["architectures"][0].get<std::string>();
            }
            const std::string modelType = result.config.value("model_type", std::string(""));
            if (result.architecture.empty() && !modelType.empty()) result.architecture = modelType;
            result.architectureFamily = architectureFamily(result.architecture, modelType);
            result.layers = intFromConfig(result.config, {"num_hidden_layers", "n_layer", "num_layers"});
            result.heads = intFromConfig(result.config, {"num_attention_heads", "n_head"});
            result.kvHeads = intFromConfig(result.config, {"num_key_value_heads", "num_kv_heads"}, result.heads);
            result.hiddenSize = intFromConfig(result.config, {"hidden_size", "n_embd", "d_model"});
            result.intermediateSize = intFromConfig(result.config, {"intermediate_size", "ffn_hidden_size", "n_inner"});
            result.vocabSize = intFromConfig(result.config, {"vocab_size"});
            result.contextLength = intFromConfig(result.config, {"max_position_embeddings", "seq_length", "n_positions", "model_max_length"});
            if (result.config.contains("pad_token_id") && result.config["pad_token_id"].is_number_integer() &&
                result.config["pad_token_id"].get<int>() < 0) {
                result.warnings.push_back("config.json 的 pad_token_id 为负数；当前 llama.cpp 转换器可能以 invalid special token 拒绝转换。建议将其设为有效 Token ID 或移除该字段。");
            }
            if (result.config.contains("torch_dtype") && result.config["torch_dtype"].is_string()) {
                result.dtype = result.config["torch_dtype"].get<std::string>();
            } else if (result.config.contains("dtype") && result.config["dtype"].is_string()) {
                result.dtype = result.config["dtype"].get<std::string>();
            }
        } catch (const std::exception& ex) {
            result.errors.push_back(std::string("config.json 解析失败: ") + ex.what());
        }
    } else {
        result.errors.push_back("缺少 config.json，无法可靠识别模型架构");
    }

    json indexJson;
    fs::path indexPath;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().u8string();
        if (endsWith(toLower(name), ".safetensors.index.json")) {
            indexPath = entry.path();
            try { indexJson = readJsonFile(indexPath); }
            catch (const std::exception& ex) { result.errors.push_back(std::string("分片索引解析失败: ") + ex.what()); }
            break;
        }
    }
    std::set<std::string> expectedShards;
    if (indexJson.is_object() && indexJson.contains("weight_map") && indexJson["weight_map"].is_object()) {
        for (auto it = indexJson["weight_map"].begin(); it != indexJson["weight_map"].end(); ++it) {
            if (it.value().is_string()) expectedShards.insert(it.value().get<std::string>());
        }
    }

    std::map<std::string, uint64_t> dtypeCounts;
    uint64_t observedShardBytes = 0;
    int validShards = 0;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().u8string();
        const std::string kind = fileKind(name);
        uint64_t size = 0;
        try { size = static_cast<uint64_t>(fs::file_size(entry.path())); } catch (...) {}
        result.totalSize += size;
        json file{{"name", name}, {"path", pathToUtf8(entry.path())}, {"size", size}, {"kind", kind}};
        if (kind == "weights") {
            ++result.shardCount;
            observedShardBytes += size;
            if (size < 16) {
                file["valid"] = false;
                file["detail"] = "文件过小，可能为空或损坏";
                result.errors.push_back("权重文件过小: " + name);
            } else {
                json header;
                std::string headerError;
                uint64_t params = 0;
                int tensors = 0;
                const bool ok = readSafetensorsHeader(entry.path(), header, headerError, params, tensors, dtypeCounts);
                result.parameterCount += params;
                result.tensorCount += tensors;
                file["valid"] = ok;
                file["tensors"] = tensors;
                file["parameters"] = params;
                if (ok) {
                    ++validShards;
                    if (header.contains("__metadata__") && header["__metadata__"].is_object()) {
                        file["metadataKeys"] = static_cast<int>(header["__metadata__"].size());
                    }
                } else {
                    file["detail"] = headerError;
                    result.errors.push_back(name + ": " + headerError);
                }
            }
        } else if (kind == "incomplete") {
            file["valid"] = false;
            file["detail"] = "发现未完成的上传或临时文件";
            result.warnings.push_back("发现临时/未完成文件: " + name);
        } else {
            file["valid"] = true;
        }
        result.files.push_back(file);
    }
    if (result.shardCount == 0) result.errors.push_back("目录中未找到 .safetensors 权重文件");
    if (result.shardCount > 0 && validShards == 0) result.errors.push_back("没有可读取的有效 Safetensors 权重文件");

    if (!expectedShards.empty()) {
        for (const auto& shard : expectedShards) {
            if (!fs::exists(directory / pathFromUtf8(shard), ec)) result.missing.push_back(shard);
        }
        if (!result.missing.empty()) result.errors.push_back("分片索引引用了缺失文件，共 " + std::to_string(result.missing.size()) + " 个");
    }

    result.tokenizer["tokenizerJson"] = fs::exists(directory / "tokenizer.json", ec);
    result.tokenizer["tokenizerConfig"] = fs::exists(directory / "tokenizer_config.json", ec);
    result.tokenizer["tokenizerModel"] = fs::exists(directory / "tokenizer.model", ec);
    result.tokenizer["specialTokensMap"] = fs::exists(directory / "special_tokens_map.json", ec);
    result.tokenizer["vocab"] = fs::exists(directory / "vocab.json", ec) && fs::exists(directory / "merges.txt", ec);
    result.tokenizerComplete = result.tokenizer.value("tokenizerJson", false) ||
                               result.tokenizer.value("tokenizerModel", false) ||
                               result.tokenizer.value("vocab", false);
    if (!result.tokenizerComplete) result.errors.push_back("缺少可用的 Tokenizer 文件（tokenizer.json、tokenizer.model 或 vocab+merges）");

    if (result.parameterCount == 0 && result.layers && result.hiddenSize) {
        const uint64_t embed = static_cast<uint64_t>(result.vocabSize) * static_cast<uint64_t>(result.hiddenSize);
        result.parameterCount = embed * 2ull + static_cast<uint64_t>(result.layers) *
            (4ull * result.hiddenSize * result.hiddenSize + 3ull * result.hiddenSize * result.intermediateSize);
        result.warnings.push_back("参数规模由 config.json 估算，未从权重张量中完整统计");
        result.report["parameterCountSource"] = "estimated-from-config";
    } else if (result.parameterCount > 0) {
        result.report["parameterCountSource"] = "safetensors-headers";
    }
    if (result.dtype.empty()) result.dtype = dominantDtype(dtypeCounts);
    if (result.architecture.empty()) result.architecture = "Unknown";
    if (result.architectureFamily.empty()) result.architectureFamily = architectureFamily(result.architecture, "");

    if (result.shardCount > 1) {
        result.report["sharded"] = true;
        result.report["observedShardBytes"] = observedShardBytes;
    }
    result.complete = result.errors.empty() && result.shardCount > 0 && result.tokenizerComplete && !result.architecture.empty() &&
                      result.architecture != "Unknown";

    json checks = json::array();
    checks.push_back(checkItem("path", "模型目录", "pass", result.path));
    checks.push_back(checkItem("config", "配置文件", fs::exists(directory / "config.json", ec) ? "pass" : "fail",
                               fs::exists(directory / "config.json", ec) ? "config.json 可读取" : "缺少 config.json"));
    checks.push_back(checkItem("weights", "权重文件", result.shardCount > 0 ? (validShards == result.shardCount ? "pass" : "fail") : "fail",
                               std::to_string(validShards) + "/" + std::to_string(result.shardCount) + " 个权重文件可读取"));
    checks.push_back(checkItem("shards", "分片完整性", result.missing.empty() ? "pass" : "fail",
                               result.missing.empty() ? "未发现缺失分片" : ("缺失 " + std::to_string(result.missing.size()) + " 个分片")));
    checks.push_back(checkItem("tokenizer", "Tokenizer", result.tokenizerComplete ? "pass" : "fail",
                               result.tokenizerComplete ? "已发现 Tokenizer 资源" : "缺少 Tokenizer 资源"));
    result.report["checks"] = checks;
    result.report["generatedAt"] = isoNow();
    return result;
}

json ModelScanner::buildInspectionReport(const ModelScanResult& scan) {
    json report = scan.report;
    report["name"] = scan.name;
    report["path"] = scan.path;
    report["architecture"] = scan.architecture;
    report["architectureFamily"] = scan.architectureFamily;
    report["dtype"] = scan.dtype;
    report["parameterCount"] = scan.parameterCount;
    report["totalSize"] = scan.totalSize;
    report["shardCount"] = scan.shardCount;
    report["tensorCount"] = scan.tensorCount;
    report["layers"] = scan.layers;
    report["heads"] = scan.heads;
    report["kvHeads"] = scan.kvHeads;
    report["hiddenSize"] = scan.hiddenSize;
    report["intermediateSize"] = scan.intermediateSize;
    report["vocabSize"] = scan.vocabSize;
    report["contextLength"] = scan.contextLength;
    report["complete"] = scan.complete;
    report["supported"] = scan.supported;
    report["tokenizerComplete"] = scan.tokenizerComplete;
    report["missing"] = scan.missing;
    report["warnings"] = scan.warnings;
    report["errors"] = scan.errors;
    report["files"] = scan.files;
    report["tokenizer"] = scan.tokenizer;
    report["configSummary"] = json{
        {"modelType", scan.config.value("model_type", "")},
        {"architectures", scan.config.value("architectures", json::array())},
        {"torchDtype", scan.config.value("torch_dtype", "")},
        {"tieWordEmbeddings", scan.config.value("tie_word_embeddings", false)},
        {"hiddenAct", scan.config.value("hidden_act", "")},
        {"ropeTheta", scan.config.value("rope_theta", 0.0)}
    };
    return report;
}

} // namespace mf
