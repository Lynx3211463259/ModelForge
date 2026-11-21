#include "modelforge/converter.hpp"

#include "modelforge/gguf_reader.hpp"
#include "modelforge/model_scanner.hpp"
#include "modelforge/process_runner.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace mf {

namespace {
struct CommandCapture {
    bool ok = false;
    int exitCode = -1;
    std::string output;
    std::string error;
};

CommandCapture runCapture(const std::string& executable, const std::vector<std::string>& args, int timeoutMs = 10000) {
    CommandCapture capture;
    ProcessOptions options;
    options.executable = executable;
    options.args = args;
    options.timeoutMs = timeoutMs;
    options.onLine = [&](const std::string& line, bool isError) {
        if (!capture.output.empty()) capture.output += "\n";
        capture.output += line;
        if (isError && capture.error.size() < 4000) {
            if (!capture.error.empty()) capture.error += "\n";
            capture.error += line;
        }
    };
    const ProcessResult result = ProcessRunner::run(options);
    capture.ok = result.started && result.exitCode == 0 && !result.timedOut;
    capture.exitCode = result.exitCode;
    if (!result.error.empty()) capture.error = result.error;
    return capture;
}

std::string gitRevision(const fs::path& root) {
    std::error_code ec;
    const fs::path gitDir = root / "llama.cpp" / ".git";
    if (!fs::exists(gitDir / "HEAD", ec)) return "";
    try {
        std::string head = trim(readTextFile(gitDir / "HEAD", 4096));
        if (startsWith(head, "ref:")) {
            const std::string ref = trim(head.substr(4));
            const fs::path refPath = gitDir / pathFromUtf8(ref);
            if (fs::exists(refPath, ec)) return trim(readTextFile(refPath, 4096)).substr(0, 12);
        }
        return head.substr(0, 12);
    } catch (...) {
        return "";
    }
}

std::vector<std::string> extractQuoted(const std::string& text) {
    std::vector<std::string> out;
    std::regex quoted("\"([^\"]+)\"");
    for (std::sregex_iterator it(text.begin(), text.end(), quoted), end; it != end; ++it) out.push_back((*it)[1]);
    return out;
}

std::vector<std::string> parseArchitectures(const std::string& script) {
    std::set<std::string> values;
    const std::string marker = "@ModelBase.register(";
    size_t pos = 0;
    while ((pos = script.find(marker, pos)) != std::string::npos) {
        const size_t end = script.find(')', pos);
        if (end == std::string::npos) break;
        for (const auto& name : extractQuoted(script.substr(pos, end - pos))) values.insert(name);
        pos = end + 1;
    }
    return std::vector<std::string>(values.begin(), values.end());
}

std::vector<std::string> parseQuantTypes(const std::string& help) {
    std::vector<std::string> types;
    std::set<std::string> seen;
    std::regex linePattern("^\\s*\\d+\\s+or\\s+([A-Za-z0-9_]+)\\s*", std::regex::icase);
    std::istringstream stream(help);
    std::string line;
    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, linePattern)) {
            const std::string type = match[1];
            if (seen.insert(type).second) types.push_back(type);
        }
    }
    if (types.empty()) {
        std::regex fallback("\\b(IQ[0-9]_[A-Z0-9_]+|Q[0-9]_[A-Z0-9_]+|TQ[12]_0|F16|BF16|F32)\\b");
        for (std::sregex_iterator it(help.begin(), help.end(), fallback), end; it != end; ++it) {
            const std::string type = (*it)[1];
            if (seen.insert(type).second) types.push_back(type);
        }
    }
    std::sort(types.begin(), types.end());
    return types;
}
} // namespace

ConverterService::ConverterService(SettingsService& settings) : settings_(settings) {}

std::vector<std::string> parseConversionTypes(const std::string& script) {
    const size_t pos = script.find("--outtype");
    if (pos == std::string::npos) return {};
    const size_t choices = script.find("choices=[", pos);
    if (choices == std::string::npos) return {};
    const size_t end = script.find(']', choices);
    if (end == std::string::npos) return {};
    std::vector<std::string> result;
    for (const auto& item : extractQuoted(script.substr(choices, end - choices))) result.push_back(item);
    return result;
}

EngineCapabilities ConverterService::detect() {
    const AppSettings s = settings_.get();
    EngineCapabilities caps;
    caps.convertScriptPath = s.convertScript;
    caps.quantizePath = s.quantizePath;
    caps.cliPath = s.llamaCliPath;

    std::error_code ec;
    caps.convertScriptAvailable = !s.convertScript.empty() && fs::is_regular_file(pathFromUtf8(s.convertScript), ec);
    if (caps.convertScriptAvailable) {
        // Prefer the official release tag recorded next to the distributed binaries;
        // the git revision is only a fallback for a developer checkout.
        std::string version;
        const fs::path runtimeRoot = s.llamaRoot.empty() ? fs::path() : pathFromUtf8(s.llamaRoot);
        if (!runtimeRoot.empty() && fs::is_regular_file(runtimeRoot / "modelforge-runtime.json", ec)) {
            const json state = readJsonFile(runtimeRoot / "modelforge-runtime.json");
            const std::string tag = state.is_object() && state.contains("tag") && state["tag"].is_string()
                                        ? state["tag"].get<std::string>()
                                        : "";
            if (!tag.empty()) version = "llama.cpp " + tag + " · 官方发行版";
        }
        if (version.empty()) {
            const std::string rev = gitRevision(settings_.root());
            version = "llama.cpp " + (rev.empty() ? std::string("local build") : rev);
        }
        caps.convertScriptVersion = version;
        try {
            const std::string script = readTextFile(pathFromUtf8(s.convertScript), 32ull * MB);
            caps.supportedArchitectures = parseArchitectures(script);
            caps.conversionTypes = parseConversionTypes(script);
            const fs::path conversionDir = pathFromUtf8(s.convertScript).parent_path() / "conversion";
            if (fs::is_directory(conversionDir, ec)) {
                std::set<std::string> merged(caps.supportedArchitectures.begin(), caps.supportedArchitectures.end());
                for (const auto& entry : fs::directory_iterator(conversionDir, ec)) {
                    if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".py") continue;
                    try {
                        for (const auto& name : parseArchitectures(readTextFile(entry.path(), 16ull * MB))) merged.insert(name);
                    } catch (...) {}
                }
                caps.supportedArchitectures.assign(merged.begin(), merged.end());
            }
        } catch (const std::exception& ex) {
            caps.issues.push_back(std::string("无法读取转换脚本: ") + ex.what());
        }
    } else {
        caps.issues.push_back("未找到 convert_hf_to_gguf.py，请检查 llama.cpp 路径");
    }

    const CommandCapture python = runCapture(s.pythonPath, {"--version"}, 5000);
    if (python.ok) {
        caps.pythonAvailable = true;
        caps.pythonVersion = trim(python.output);
        const CommandCapture deps = runCapture(s.pythonPath, {"-c",
            "import torch,numpy,transformers;print('torch='+torch.__version__+' numpy='+numpy.__version__+' transformers='+transformers.__version__)"}, 30000);
        caps.pythonDependenciesReady = deps.ok;
        if (!deps.ok) {
            std::string reason = trim(deps.error);
            if (reason.empty()) reason = trim(deps.output);
            if (reason.size() > 500) reason = reason.substr(reason.size() - 500);
            caps.pythonDependencyError = reason.empty() ? "无法导入 torch / numpy / transformers" : reason;
            caps.issues.push_back("Python 转换依赖不可用：" + caps.pythonDependencyError);
        }
    } else {
        caps.pythonDependencyError = "Python 不可用";
        caps.issues.push_back("无法执行 Python：" + s.pythonPath);
    }

    caps.quantizeAvailable = !s.quantizePath.empty() && fs::is_regular_file(pathFromUtf8(s.quantizePath), ec);
    if (caps.quantizeAvailable) {
        const CommandCapture help = runCapture(s.quantizePath, {"--help"}, 10000);
        if (!help.output.empty()) {
            caps.quantTypes = parseQuantTypes(help.output);
            caps.quantizeVersion = "llama-quantize · " + pathToUtf8(pathFromUtf8(s.quantizePath).filename());
        }
        if (caps.quantTypes.empty()) {
            caps.issues.push_back("量化工具存在，但未能解析受支持的量化类型");
            caps.quantizeAvailable = false;
        }
    } else {
        caps.issues.push_back("未找到 llama-quantize，可执行文件路径需在设置中配置");
    }

    caps.cliAvailable = !s.llamaCliPath.empty() && fs::is_regular_file(pathFromUtf8(s.llamaCliPath), ec);
    caps.serverAvailable = !s.llamaServerPath.empty() && fs::is_regular_file(pathFromUtf8(s.llamaServerPath), ec);
    if (!s.llamaBenchPath.empty() && fs::is_regular_file(pathFromUtf8(s.llamaBenchPath), ec)) {
        caps.benchAvailable = true;
        caps.benchPath = s.llamaBenchPath;
    }
    if (!s.llamaPplPath.empty() && fs::is_regular_file(pathFromUtf8(s.llamaPplPath), ec)) {
        caps.pplAvailable = true;
        caps.pplPath = s.llamaPplPath;
    }
    if (caps.conversionTypes.empty() && caps.convertScriptAvailable) {
        caps.issues.push_back("未能从转换脚本解析输出精度参数");
    }
    return caps;
}

EngineCapabilities ConverterService::capabilities(bool refresh) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    const int64_t now = nowMs();
    if (!refresh && cachedAt_ > 0 && now - cachedAt_ < 30000) return cached_;
    cached_ = detect();
    cachedAt_ = now;
    return cached_;
}

json ConverterService::compatibility(const ModelScanResult& scan) {
    const EngineCapabilities caps = capabilities();
    json result{{"supported", false}, {"match", "none"}, {"architecture", scan.architecture},
                {"reason", ""}, {"engineReady", caps.convertScriptAvailable && caps.pythonDependenciesReady}};
    if (!caps.convertScriptAvailable) {
        result["reason"] = "转换脚本不可用";
        return result;
    }
    if (!caps.pythonDependenciesReady) {
        result["reason"] = "Python 转换依赖尚未安装";
        return result;
    }
    if (scan.architecture.empty() || scan.architecture == "Unknown") {
        result["reason"] = "无法从 config.json 识别模型架构";
        return result;
    }
    const std::string target = toLower(scan.architecture);
    for (const auto& name : caps.supportedArchitectures) {
        if (toLower(name) == target) {
            result["supported"] = true;
            result["match"] = "exact";
            result["reason"] = "转换脚本注册了该架构";
            return result;
        }
    }
    const std::string modelType = toLower(scan.config.value("model_type", std::string("")));
    if (!modelType.empty() && modelType != "unknown") {
        for (const auto& name : caps.supportedArchitectures) {
            if (toLower(name).find(modelType) != std::string::npos) {
                result["supported"] = true;
                result["match"] = "heuristic";
                result["reason"] = "通过 model_type 与脚本注册名进行启发式匹配，请在转换前确认";
                return result;
            }
        }
    }
    // Custom multimodal checkpoints (speech/vision omni models) frequently use a private
    // model_type and split sub-configs. llama.cpp does not implement those graphs, and a
    // version upgrade alone will not help, so report the concrete reason instead of a
    // generic "unsupported architecture".
    const json& config = scan.config;
    std::vector<std::string> components;
    struct ComponentHint { const char* key; const char* label; };
    const ComponentHint hints[] = {
        {"audio_decoder_config", "音频解码器"},
        {"audio_config", "音频编码器"},
        {"vision_config", "视觉编码器"},
        {"thinker_config", "Thinker 子模型"},
        {"talker_config", "Talker 子模型"},
        {"text_config", "分离的文本子配置"}
    };
    for (const auto& hint : hints) {
        if (config.contains(hint.key)) components.push_back(hint.label);
    }
    if (!components.empty()) {
        std::vector<std::string> related;
        for (const auto& name : caps.supportedArchitectures) {
            const std::string lower = toLower(name);
            if (lower.find("qwen3omni") != std::string::npos || lower.find("omni") != std::string::npos) {
                related.push_back(name);
                if (related.size() >= 3) break;
            }
        }
        std::string reason = "该模型使用自定义/多模态复合架构（model_type=" + scan.architecture +
                             "，包含 " + join(components, "、") + "），当前 llama.cpp 转换脚本未实现该计算图";
        if (!related.empty()) reason += "；脚本仅注册了 " + join(related, "、") + "，权重命名与该架构不同";
        reason += "。这不是升级 llama.cpp 就能解决的兼容性问题";
        result["code"] = "ARCH_UNSUPPORTED_CUSTOM";
        result["reason"] = reason;
        result["details"] = json{{"modelType", scan.architecture}, {"components", components},
                                 {"relatedArchitectures", related}};
        return result;
    }
    result["code"] = "ARCH_UNSUPPORTED";
    result["reason"] = "当前转换脚本未注册架构 " + scan.architecture + "；升级 llama.cpp 或使用受支持模型";
    return result;
}

double ConverterService::bitsPerWeight(const std::string& type) {
    static const std::map<std::string, double> table = {
        {"F32", 32.0}, {"F16", 16.0}, {"BF16", 16.0}, {"Q8_0", 8.5}, {"Q6_K", 6.5625},
        {"Q5_K_M", 5.5}, {"Q5_K", 5.5}, {"Q5_K_S", 5.5}, {"Q5_1", 5.5}, {"Q5_0", 5.25},
        {"Q4_K_M", 4.8}, {"Q4_K", 4.8}, {"Q4_K_S", 4.5}, {"Q4_1", 4.5}, {"Q4_0", 4.5},
        {"IQ4_XS", 4.25}, {"IQ4_NL", 4.5}, {"Q3_K_M", 3.9}, {"Q3_K", 3.9}, {"Q3_K_S", 3.4},
        {"IQ3_M", 3.66}, {"IQ3_S", 3.44}, {"IQ3_XXS", 3.06}, {"Q2_K", 3.0}, {"Q2_K_S", 2.96},
        {"IQ2_M", 2.7}, {"IQ2_S", 2.5}, {"IQ2_XS", 2.31}, {"IQ2_XXS", 2.06}, {"IQ1_M", 1.75},
        {"IQ1_S", 1.56}, {"TQ1_0", 1.69}, {"TQ2_0", 2.06}
    };
    const auto it = table.find(type);
    return it == table.end() ? 0.0 : it->second;
}

std::string ConverterService::typeDescription(const std::string& type) {
    if (type == "F32") return "32 位浮点，最高保真，体积最大";
    if (type == "F16" || type == "BF16") return "16 位浮点，适合作为量化中间格式";
    if (type == "Q8_0") return "8 位量化，质量接近原始，体积约为 F16 的 53%";
    if (type == "Q6_K") return "6 位 K 量化，高质量";
    if (type == "Q5_K_M") return "5 位 K 混合量化，质量与体积平衡";
    if (type == "Q4_K_M") return "4 位 K 混合量化，通用推荐";
    if (type == "Q3_K_M") return "3 位 K 量化，体积较小";
    if (type == "Q2_K") return "2 位 K 量化，极限压缩，质量损失明显";
    if (startsWith(type, "IQ")) return "importance matrix 量化，通常需要校准数据";
    if (startsWith(type, "TQ")) return "ternary 量化，需要相应运行时支持";
    return "llama.cpp 支持的量化类型";
}

json ConverterService::estimateList(const ModelScanResult& scan) {
    const EngineCapabilities caps = capabilities();
    const bool haveParams = scan.parameterCount > 0;
    const double inputBytesPerParam = [&]() {
        const std::string dtype = toLower(scan.dtype);
        if (dtype.find("32") != std::string::npos) return 4.0;
        if (dtype.find("16") != std::string::npos || dtype.find("bf16") != std::string::npos) return 2.0;
        if (dtype.find("8") != std::string::npos) return 1.0;
        return 2.0;
    }();
    const uint64_t sourceWeightBytes = haveParams
        ? static_cast<uint64_t>(static_cast<double>(scan.parameterCount) * inputBytesPerParam)
        : scan.totalSize;
    const std::vector<std::string> types = {"F16", "BF16", "Q8_0", "Q6_K", "Q5_K_M", "Q4_K_M", "Q3_K_M", "Q2_K"};
    std::set<std::string> availableQuant;
    for (const auto& type : caps.quantTypes) availableQuant.insert(toLower(type));

    json list = json::array();
    for (const auto& type : types) {
        const double bits = bitsPerWeight(type);
        uint64_t bytes = 0;
        if (haveParams && bits > 0) {
            const double rawBytes = static_cast<double>(scan.parameterCount) * bits / 8.0;
            bytes = static_cast<uint64_t>(rawBytes * 1.01 + std::max<uint64_t>(16ull * 1024ull, static_cast<uint64_t>(rawBytes * 0.002)));
        } else if (sourceWeightBytes > 0) {
            const double ratio = bits / (inputBytesPerParam * 8.0);
            const double rawBytes = static_cast<double>(sourceWeightBytes) * ratio;
            bytes = static_cast<uint64_t>(rawBytes * 1.01 + std::max<uint64_t>(16ull * 1024ull, static_cast<uint64_t>(rawBytes * 0.002)));
        }
        bool available = true;
        std::string unavailableReason;
        if (type == "F16" || type == "BF16" || type == "F32") {
            available = std::find(caps.conversionTypes.begin(), caps.conversionTypes.end(), toLower(type)) != caps.conversionTypes.end() ||
                        caps.conversionTypes.empty();
        } else {
            available = availableQuant.count(toLower(type)) > 0;
            if (!available) unavailableReason = "未在当前 llama-quantize 中检测到该类型";
        }
        if (!available && unavailableReason.empty()) unavailableReason = "转换工具未提供该输出类型";
        list.push_back(json{
            {"type", type}, {"bitsPerWeight", bits}, {"estimatedSize", bytes}, {"available", available},
            {"reason", unavailableReason}, {"description", typeDescription(type)},
            {"estimatedRuntimeMemory", static_cast<uint64_t>(static_cast<double>(bytes) * 1.2 + 128ull * MB)}
        });
    }
    return list;
}

json ConverterService::estimates(const ModelScanResult& scan) {
    const auto list = estimateList(scan);
    uint64_t largest = 0;
    for (const auto& item : list) largest = std::max<uint64_t>(largest, item.value("estimatedSize", 0ull));
    return json{
        {"parameterCount", scan.parameterCount}, {"sourceSize", scan.totalSize},
        {"sourceDtype", scan.dtype}, {"options", list},
        {"method", "按参数规模 × 每权重比特数 + 1% 开销 + 32 MiB 头部余量估算；实际大小会因嵌入层、输出层和张量混合策略不同而变化。"},
        {"maximumEstimatedSize", largest}
    };
}

json ConverterService::recommend(const ModelScanResult& scan, const std::string& mode,
                                 uint64_t availableMemory, uint64_t availableVram) {
    if (availableMemory == 0) availableMemory = availableSystemMemory();
    const uint64_t usableMemory = availableMemory > 0 ? static_cast<uint64_t>(availableMemory * 0.8) : totalSystemMemory() * 3 / 4;
    json estimatesJson = estimates(scan);
    json options = estimatesJson["options"];
    const bool quality = mode == "quality";
    const bool compact = mode == "size" || mode == "compact";
    std::vector<std::string> preference = quality
        ? std::vector<std::string>{"Q6_K", "Q8_0", "Q5_K_M", "Q4_K_M", "Q3_K_M"}
        : compact ? std::vector<std::string>{"Q3_K_M", "Q2_K", "Q4_K_M", "Q5_K_M", "Q6_K"}
                  : std::vector<std::string>{"Q4_K_M", "Q5_K_M", "Q3_K_M", "Q6_K", "Q2_K"};

    json candidates = json::array();
    json selected = nullptr;
    for (const auto& preferenceType : preference) {
        for (const auto& option : options) {
            if (option.value("type", "") == preferenceType && option.value("available", false)) {
                candidates.push_back(option);
                if (selected.is_null()) selected = option;
            }
        }
    }
    if (selected.is_null()) {
        for (const auto& option : options) {
            if (option.value("available", false)) {
                candidates.push_back(option);
                if (selected.is_null()) selected = option;
            }
        }
    }
    json result{
        {"mode", mode}, {"availableMemory", availableMemory}, {"usableMemoryBudget", usableMemory},
        {"availableVram", availableVram}, {"recommended", selected}, {"alternatives", candidates},
        {"method", "优先匹配质量意图，再参考当前可用内存的 80% 预算；运行内存为启发式估算，不包含完整 KV Cache 实测。"},
        {"resourceWarning", ""}
    };
    if (selected.is_null()) {
        result["reason"] = "没有检测到可执行的量化类型，请先配置并验证 llama-quantize";
        return result;
    }
    const uint64_t runtime = selected.value("estimatedRuntimeMemory", 0ull);
    result["reason"] = quality ? "质量优先：选择保留更多权重精度的可用方案" :
                       compact ? "体积优先：选择更小、可在有限内存中运行的可用方案" :
                                 "平衡模式：Q4_K_M 通常是通用本地推理的体积与质量折中点";
    if (usableMemory > 0 && runtime > usableMemory) {
        result["resourceWarning"] = "推荐模型的估算运行内存超过当前可用内存预算，运行可能失败。";
    }
    return result;
}

fs::path ConverterService::finalPath(const ConversionParams& params) const {
    fs::path base = params.outputDir.empty() ? settings_.outputDir() : pathFromUtf8(params.outputDir);
    std::string name = sanitizeFileName(params.outputName.empty() ? "model" : params.outputName);
    if (endsWith(toLower(name), ".gguf")) name = name.substr(0, name.size() - 5);
    return base / (name + ".gguf");
}

fs::path ConverterService::intermediatePath(const ConversionParams& params) const {
    fs::path base = params.outputDir.empty() ? settings_.outputDir() : pathFromUtf8(params.outputDir);
    std::string name = sanitizeFileName(params.outputName.empty() ? "model" : params.outputName);
    if (endsWith(toLower(name), ".gguf")) name = name.substr(0, name.size() - 5);
    if (params.quantType.empty()) return base / (name + ".gguf");
    return base / (name + ".intermediate." + params.outType + ".gguf");
}

bool ConverterService::validateParams(ConversionParams& params, std::string& code, std::string& error) {
    const EngineCapabilities caps = capabilities();
    std::error_code ec;
    if (params.inputPath.empty()) { code = "INPUT_REQUIRED"; error = "必须选择模型目录"; return false; }
    params.inputPath = pathToUtf8(fs::weakly_canonical(pathFromUtf8(params.inputPath), ec));
    if (!fs::is_directory(pathFromUtf8(params.inputPath), ec)) { code = "MODEL_PATH_NOT_FOUND"; error = "模型目录不存在"; return false; }
    if (!fs::exists(pathFromUtf8(params.inputPath) / "config.json", ec)) { code = "CONFIG_MISSING"; error = "模型目录缺少 config.json"; return false; }
    if (!caps.convertScriptAvailable) { code = "CONVERTER_MISSING"; error = "转换脚本不存在，请先在设置中配置"; return false; }
    if (!caps.pythonDependenciesReady) { code = "PYTHON_ENV_INVALID"; error = "Python 转换依赖不可用: " + caps.pythonDependencyError; return false; }
    const std::string outType = toLower(params.outType.empty() ? "f16" : params.outType);
    if (outType != "f32" && outType != "f16" && outType != "bf16") {
        code = "OUTTYPE_UNSUPPORTED";
        error = "第一阶段输出精度只支持 f32、f16、bf16；量化类型应在量化阶段选择";
        return false;
    }
    if (!params.quantType.empty()) {
        if (!caps.quantizeAvailable) { code = "QUANTIZER_MISSING"; error = "量化工具不可用，请检查 llama-quantize 路径"; return false; }
        const std::string target = toLower(params.quantType);
        const bool supported = std::any_of(caps.quantTypes.begin(), caps.quantTypes.end(),
            [&](const std::string& t) { return toLower(t) == target; });
        if (!supported) { code = "QUANT_TYPE_UNSUPPORTED"; error = "当前量化工具不支持类型 " + params.quantType; return false; }
    }
    if (params.threads < 0 || params.threads > 512) { code = "INVALID_THREADS"; error = "线程数必须在 0-512 之间"; return false; }
    fs::path outDir = params.outputDir.empty() ? settings_.outputDir() : pathFromUtf8(params.outputDir);
    if (!ensureDirectory(outDir, error)) { code = "OUTPUT_DIR_UNWRITABLE"; return false; }
    if (!directoryWritable(outDir)) { code = "OUTPUT_DIR_UNWRITABLE"; error = "输出目录不可写: " + pathToUtf8(outDir); return false; }
    params.outputDir = pathToUtf8(fs::absolute(outDir));
    params.outputName = sanitizeFileName(params.outputName.empty() ? "model" : params.outputName);
    if (endsWith(toLower(params.outputName), ".gguf")) params.outputName = params.outputName.substr(0, params.outputName.size() - 5);
    params.outType = outType;
    if (params.threads == 0) params.threads = settings_.get().threads;
    const fs::path outputPath = finalPath(params);
    if (fs::exists(outputPath, ec) && !params.overwrite) {
        code = "OUTPUT_EXISTS";
        error = "输出文件已存在，请启用覆盖或更改名称: " + pathToUtf8(outputPath);
        return false;
    }
    if (fs::exists(pathFromUtf8(params.inputPath) / outputPath.filename(), ec)) {
        code = "OUTPUT_CONFLICT";
        error = "输出文件不能与输入模型目录中的文件同名";
        return false;
    }
    return true;
}

json ConverterService::commandPreview(const ConversionParams& params) {
    const AppSettings s = settings_.get();
    const fs::path intermediate = intermediatePath(params);
    const fs::path output = finalPath(params);
    std::vector<std::string> convertArgs = {s.convertScript, "--outtype", params.outType, "--outfile", pathToUtf8(intermediate)};
    if (params.useTempFile) convertArgs.push_back("--use-temp-file");
    if (params.noLazy) convertArgs.push_back("--no-lazy");
    if (!params.modelName.empty()) { convertArgs.push_back("--model-name"); convertArgs.push_back(sanitizeSingleLine(params.modelName, 200)); }
    convertArgs.push_back(params.inputPath);

    json result{
        {"convert", "python " + joinCommandForDisplay(s.convertScript, std::vector<std::string>(convertArgs.begin() + 1, convertArgs.end()),
#ifdef _WIN32
            true
#else
            false
#endif
        )},
        {"quantize", nullptr}, {"validate", "ModelForge GGUF 结构校验（内置）"}
    };
    if (!params.quantType.empty()) {
        std::vector<std::string> quantArgs = {pathToUtf8(intermediate), pathToUtf8(output), params.quantType, std::to_string(params.threads)};
        result["quantize"] = joinCommandForDisplay(s.quantizePath, quantArgs,
#ifdef _WIN32
            true
#else
            false
#endif
        );
    }
    if (params.deepVerify && !s.llamaCliPath.empty()) {
        result["deepValidate"] = joinCommandForDisplay(s.llamaCliPath,
            {"-m", pathToUtf8(output), "-p", "Hello", "-n", "1", "--no-warmup", "--simple-io", "-st"},
#ifdef _WIN32
            true
#else
            false
#endif
        );
    }
    return result;
}

} // namespace mf
