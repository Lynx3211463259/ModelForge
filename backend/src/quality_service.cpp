#include "modelforge/quality_service.hpp"

#include "modelforge/logger.hpp"
#include "modelforge/process_runner.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>

namespace mf {

namespace {

// Fixed English prose used only when the user has not supplied a corpus. It is NOT
// a standard benchmark: results are comparable between quantizations of the same
// model, but they are not comparable with published wikitext-2 numbers.
const char* kBuiltinSample = R"SAMPLE(The practice of running language models on a local machine has changed a great deal
in a short time. A few years ago a model small enough to fit on a desk was little
more than a curiosity, useful for experiments and nothing else. Today a modest
laptop can hold a model that writes code, answers questions about a document, or
translates a paragraph while the user waits. The change did not come from a single
breakthrough. It came from many small improvements in architecture, in training
data, and above all in the way numbers are stored and moved.

A neural network spends most of its memory on weights. Each weight is a number,
and the obvious way to store a number is to keep every bit that a floating point
format provides. That is simple and accurate, but it is also wasteful. When a
model has billions of weights, the difference between four bytes per weight and
two bytes per weight decides whether it runs at all. Quantization is the craft of
reducing that cost while keeping the behaviour of the model close to the original.
It is not compression in the ordinary sense, because the goal is not to reproduce
the bytes exactly. The goal is to reproduce the answers.

The simplest schemes round every weight to the nearest value on a small grid. More
careful schemes keep a scale for a block of weights, so that unusual blocks do not
lose all of their detail. The best schemes notice that not every part of a model
matters equally. Some layers carry information that a small error destroys, and
those layers deserve more precision. Others tolerate a coarse grid with no visible
harm. A mixed recipe that gives different precision to different tensors often
beats a uniform recipe at the same size, which is why the practical formats have
long names with several letters in them instead of a single number.

Measuring the damage is the hard part. A model can still produce fluent sentences
after aggressive quantization, and fluency is a poor guide to quality. What is
needed is a number that goes up when the model becomes less certain about real
text. Perplexity is exactly that number. It is computed by feeding a corpus to the
model one token at a time and asking how surprised the model was by each token.
A low value means the model expected the text. A high value means it was guessing.
Because the calculation uses only the model's own probabilities, it does not need
reference answers, and it does not need a human to judge the output.

The catch is that perplexity depends on the corpus. A model trained on code will
look excellent on code and mediocre on novels. A model trained on novels will show
the opposite pattern. Numbers from different corpora therefore cannot be compared,
and even numbers from the same corpus can drift when the context length or the
tokenizer changes. Good practice is to fix the corpus, fix the context, and compare
only models that were evaluated the same way. Under those conditions perplexity is
a sharp instrument. It notices a bad quantization long before a reader does, and it
costs far less than a full evaluation by hand.

None of this replaces a real task. Perplexity tells you that a model has become
less certain; it does not tell you that the model now fails at the task you care
about. A sensible workflow uses both. Start with a perplexity sweep across the
formats you are considering, discard the ones that lose too much, and then run a
handful of real prompts against the survivors. Keep the numbers, the corpus and
the tool version next to the files, so that a result from last month can still be
explained today. Reproducibility is not paperwork. It is what turns a lucky
observation into a decision.
)SAMPLE";

// Paths reach the quality service from JSON requests (forward slashes) and from the
// database (native separators). Normalize before storing or comparing so the same
// artifact is not treated as two different files.
std::string pathKey(std::string value) {
#ifdef _WIN32
    std::replace(value.begin(), value.end(), '/', '\\');
#endif
    return value;
}

std::string inferQuantFromName(const std::string& path) {
    const std::string name = toLower(pathFromUtf8(path).filename().string());
    static const std::regex pattern(R"((iq[0-9]_[a-z0-9_]+|q[0-9]_[a-z0-9_]+|q[0-9]_[0-9]|f16|bf16|f32))");
    std::smatch match;
    if (std::regex_search(name, match, pattern)) return toUpper(match[1].str());
    return "";
}

} // namespace

QualityService::QualityService(Database& db, SettingsService& settings, SystemService& system, EventHub& events)
    : db_(db), settings_(settings), system_(system), events_(events) {}

QualityService::~QualityService() { stop(); }

void QualityService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelFlag_) cancelFlag_->store(true);
    }
    if (worker_.joinable()) worker_.join();
    running_ = false;
}

json QualityService::dataset() const {
    json result = json::object();
    const AppSettings s = settings_.get();
    const fs::path configured = s.pplDataset.empty() ? fs::path() : pathFromUtf8(s.pplDataset);
    std::error_code ec;
    if (!configured.empty() && fs::is_regular_file(configured, ec)) {
        result = json{{"path", pathToUtf8(configured)}, {"kind", "configured"},
                      {"bytes", static_cast<uint64_t>(fs::file_size(configured, ec))},
                      {"standard", false}, {"exists", true}};
        return result;
    }
    const fs::path dataDir = settings_.dataDir();
    for (const auto& candidate : {dataDir / "datasets" / "wikitext-2-raw" / "wiki.test.raw",
                                  dataDir / "datasets" / "wiki.test.raw",
                                  dataDir / "quality" / "dataset.txt"}) {
        if (fs::is_regular_file(candidate, ec)) {
            result = json{{"path", pathToUtf8(candidate)}, {"kind", "configured"},
                          {"bytes", static_cast<uint64_t>(fs::file_size(candidate, ec))},
                          {"standard", candidate.filename() == "wiki.test.raw"}, {"exists", true}};
            return result;
        }
    }
    const fs::path sample = dataDir / "quality" / "sample.txt";
    result = json{{"path", pathToUtf8(sample)}, {"kind", "builtin-sample"},
                  {"bytes", fs::is_regular_file(sample, ec) ? static_cast<uint64_t>(fs::file_size(sample, ec)) : 0},
                  {"standard", false}, {"exists", fs::is_regular_file(sample, ec)},
                  {"hint", "内置示例文本不是标准测试集，只用于同一模型不同量化之间的相对比较；如需与公开数值对比，请把 wiki.test.raw 放到 data/datasets/。"}};
    return result;
}

bool QualityService::ensureDataset(std::string& path, std::string& kind, std::string& error) {
    const json info = dataset();
    if (info.value("kind", "") == "builtin-sample") {
        const fs::path sample = pathFromUtf8(info.value("path", ""));
        std::error_code ec;
        if (!fs::is_regular_file(sample, ec)) {
            fs::create_directories(sample.parent_path(), ec);
            if (ec || !writeTextFile(sample, kBuiltinSample)) {
                error = "无法写入内置评测文本 " + pathToUtf8(sample);
                return false;
            }
        }
        path = pathToUtf8(sample);
        kind = "builtin-sample";
        return true;
    }
    path = info.value("path", "");
    kind = info.value("kind", "configured");
    if (path.empty()) {
        error = "未找到可用的评测文本";
        return false;
    }
    return true;
}

bool QualityService::run(const json& options, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        error = "已有质量评测任务正在进行";
        return false;
    }
    const AppSettings s = settings_.get();
    std::error_code ec;
    if (s.llamaPplPath.empty() || !fs::is_regular_file(pathFromUtf8(s.llamaPplPath), ec)) {
        error = "llama-perplexity 不可用，请先在“运行引擎”页安装或更新官方 llama.cpp";
        return false;
    }

    std::vector<json> targets;
    std::string modelId = options.value("modelId", std::string(""));
    if (options.contains("targets") && options["targets"].is_array()) {
        for (const auto& item : options["targets"]) {
            if (!item.is_object()) continue;
            const std::string path = item.value("path", std::string(""));
            if (path.empty()) continue;
            targets.push_back(json{{"path", path},
                                   {"versionId", item.value("versionId", std::string(""))},
                                   {"quantType", item.value("quantType", std::string(""))}});
        }
    } else if (options.contains("path") && options["path"].is_string()) {
        targets.push_back(json{{"path", options["path"].get<std::string>()},
                               {"versionId", ""}, {"quantType", options.value("quantType", std::string(""))}});
    }
    if (targets.empty() && !modelId.empty()) {
        for (const auto& row : db_.query(
                 "SELECT id,path,quant_type FROM model_versions WHERE model_id=? ORDER BY created_at ASC", {modelId})) {
            targets.push_back(json{{"path", row.value("path", "")},
                                   {"versionId", row.value("id", "")},
                                   {"quantType", row.value("quant_type", "")}});
        }
    }
    if (targets.empty()) {
        error = "没有可评测的 GGUF 文件；请先完成转换或指定 targets";
        return false;
    }
    if (targets.size() > 16) {
        error = "单次最多评测 16 个 GGUF 文件";
        return false;
    }

    std::string datasetPath;
    std::string datasetKind;
    if (!ensureDataset(datasetPath, datasetKind, error)) return false;

    const int context = std::max(128, std::min(options.value("context", 512), 4096));
    int threads = options.value("threads", s.threads);
    if (threads < 0 || threads > 512) threads = s.threads;

    job_ = Job{};
    job_.status = "running";
    job_.total = static_cast<int>(targets.size());
    job_.startedAt = nowMs();
    job_.message = "开始评测 " + std::to_string(targets.size()) + " 个 GGUF 文件";
    job_.logs.push_back(json{{"level", "INFO"}, {"message", "评测文本：" + datasetPath + "（" + datasetKind + "）"}});
    cancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    running_ = true;
    if (worker_.joinable()) worker_.join();
    const std::shared_ptr<std::atomic<bool>> flag = cancelFlag_;
    worker_ = std::thread([this, targets, datasetPath, datasetKind, context, threads, modelId, flag] {
        workerMain(targets, datasetPath, datasetKind, context, threads, modelId);
    });
    events_.publish(json{{"type", "ppl"}, {"status", "running"}, {"total", job_.total}});
    return true;
}

bool QualityService::cancel(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !cancelFlag_) {
        error = "当前没有正在运行的质量评测";
        return false;
    }
    cancelFlag_->store(true);
    job_.message = "已请求取消，等待当前进程退出";
    return true;
}

json QualityService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json logs = json::array();
    const size_t start = job_.logs.size() > 200 ? job_.logs.size() - 200 : 0;
    for (size_t i = start; i < job_.logs.size(); ++i) logs.push_back(job_.logs[i]);
    return json{{"status", job_.status},
                {"progress", job_.progress},
                {"total", job_.total},
                {"completed", job_.completed},
                {"currentTarget", job_.currentTarget},
                {"message", job_.message},
                {"errorCode", job_.errorCode},
                {"errorMessage", job_.errorMessage},
                {"startedAt", job_.startedAt},
                {"finishedAt", job_.finishedAt},
                {"results", job_.results},
                {"logs", logs},
                {"dataset", dataset()}};
}

json QualityService::results(int limit) {
    json items = json::array();
    limit = std::max(1, std::min(limit, 500));
    for (const auto& row : db_.query("SELECT * FROM ppl_results ORDER BY created_at DESC LIMIT ?", {limit})) {
        json raw = json::object();
        try { raw = json::parse(row.value("raw_json", "{}")); } catch (...) {}
        items.push_back(json{
            {"id", row.value("id", "")}, {"modelId", row.value("model_id", "")},
            {"versionId", row.value("version_id", "")}, {"quantType", row.value("quant_type", "")},
            {"path", row.value("gguf_path", "")}, {"datasetPath", row.value("dataset_path", "")},
            {"datasetKind", row.value("dataset_kind", "")},
            {"datasetBytes", row.value("dataset_bytes", 0ll)},
            {"context", row.value("context", 0)}, {"chunks", row.value("chunks", 0)},
            {"tokens", row.value("tokens", 0)}, {"ppl", row.value("ppl", 0.0)},
            {"pplStderr", row.value("ppl_stderr", 0.0)},
            {"durationMs", row.value("duration_ms", 0ll)}, {"cpuMs", row.value("cpu_ms", 0ll)},
            {"peakMemory", row.value("peak_memory", 0ll)},
            {"createdAt", row.value("created_at", "")},
            {"finalLine", raw.value("finalLine", "")}});
    }
    return items;
}

bool QualityService::remove(const std::string& id, std::string& error) {
    const auto rows = db_.query("SELECT id FROM ppl_results WHERE id=?", {id});
    if (rows.empty()) { error = "评测结果不存在"; return false; }
    db_.query("DELETE FROM ppl_results WHERE id=?", {id});
    return true;
}

std::string QualityService::versionIdForPath(const std::string& path) const {
    const auto rows = db_.query("SELECT id FROM model_versions WHERE path=? ORDER BY created_at DESC LIMIT 1", {pathKey(path)});
    return rows.empty() ? "" : rows[0].value("id", "");
}

std::string QualityService::modelIdForPath(const std::string& path) const {
    const auto rows = db_.query("SELECT id FROM models WHERE path=? LIMIT 1", {pathKey(path)});
    if (!rows.empty()) return rows[0].value("id", "");
    const auto version = db_.query("SELECT model_id FROM model_versions WHERE path=? LIMIT 1", {pathKey(path)});
    return version.empty() ? "" : version[0].value("model_id", "");
}

void QualityService::appendLog(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    job_.logs.push_back(json{{"level", level}, {"message", message}});
    if (job_.logs.size() > 600) job_.logs.erase(job_.logs.begin(), job_.logs.begin() + 200);
}

json QualityService::evaluateOne(const json& target, const std::string& datasetPath, const std::string& datasetKind,
                                 int context, int threads, const std::string& modelId,
                                 const std::shared_ptr<std::atomic<bool>>& cancel,
                                 std::string& errorCode, std::string& errorMessage) {
    const std::string path = pathKey(target.value("path", ""));
    const fs::path gguf = pathFromUtf8(path);
    std::error_code ec;
    if (!fs::is_regular_file(gguf, ec)) {
        errorCode = "FILE_NOT_FOUND";
        errorMessage = "GGUF 文件不存在：" + path;
        return nullptr;
    }
    const AppSettings s = settings_.get();
    ProcessOptions opt;
    opt.executable = s.llamaPplPath;
    opt.args = {"-m", path, "-f", datasetPath, "-c", std::to_string(context)};
    if (threads > 0) opt.args.insert(opt.args.end(), {"-t", std::to_string(threads)});
    opt.cancelFlag = cancel.get();
    opt.timeoutMs = 3600000;

    std::string finalLine;
    double ppl = 0.0, stderrValue = 0.0;
    int chunks = 0;
    const std::regex finalPattern(R"(final estimate:\s*ppl\s*=\s*([0-9]+(?:\.[0-9]+)?)\s*\+/-\s*([0-9]+(?:\.[0-9]+)?))",
                                  std::regex::icase);
    const std::regex chunkPattern(R"(\[([0-9]+)\])");
    int64_t peakMemory = 0;
    int64_t cpuMs = 0;
    std::vector<std::string> tail;

    opt.onLine = [&](const std::string& line, bool) {
        tail.push_back(line);
        if (tail.size() > 60) tail.erase(tail.begin(), tail.begin() + 20);
        std::smatch match;
        if (std::regex_search(line, match, finalPattern)) {
            ppl = std::atof(match[1].str().c_str());
            stderrValue = std::atof(match[2].str().c_str());
            finalLine = line;
        } else {
            // One line can contain many chunk markers ("[1]..,[2]..,[3].."), so take the
            // highest index rather than the first match.
            for (std::sregex_iterator it(line.begin(), line.end(), chunkPattern), end; it != end; ++it) {
                chunks = std::max(chunks, std::atoi((*it)[1].str().c_str()));
            }
        }
        appendLog("INFO", line.substr(0, 400));
    };
    opt.onSample = [&](const ProcessSample& sample) {
        peakMemory = std::max<int64_t>(peakMemory, static_cast<int64_t>(
            sample.peakMemoryBytes ? sample.peakMemoryBytes : sample.workingSetBytes));
        cpuMs = std::max(cpuMs, sample.cpuTimeMs);
    };

    const ProcessResult result = ProcessRunner::run(opt);
    if (result.canceled || (cancel && cancel->load())) {
        errorCode = "CANCELLED";
        errorMessage = "用户取消了质量评测";
        return nullptr;
    }
    if (!result.started || result.exitCode != 0) {
        errorCode = "PPL_FAILED";
        errorMessage = "llama-perplexity 运行失败，退出码 " + std::to_string(result.exitCode);
        return nullptr;
    }
    if (ppl <= 0.0) {
        errorCode = "PPL_UNPARSED";
        errorMessage = "未能从输出中解析困惑度；请确认评测文本足够长且模型可正常加载";
        return nullptr;
    }

    const std::string id = newId("ppl");
    std::string quantType = target.value("quantType", "");
    if (quantType.empty()) quantType = inferQuantFromName(path);
    std::string versionId = target.value("versionId", "");
    if (versionId.empty()) versionId = versionIdForPath(path);
    std::string resolvedModel = modelId.empty() ? modelIdForPath(path) : modelId;

    const uint64_t datasetBytes = fs::is_regular_file(pathFromUtf8(datasetPath), ec)
        ? static_cast<uint64_t>(fs::file_size(pathFromUtf8(datasetPath), ec)) : 0;
    json raw{{"finalLine", finalLine},
             {"tail", tail},
             {"command", joinCommandForDisplay(s.llamaPplPath, opt.args, true)},
             {"exitCode", result.exitCode},
             {"durationMs", result.durationMs}};
    db_.query("INSERT INTO ppl_results(id,model_id,version_id,quant_type,gguf_path,dataset_path,dataset_kind,"
              "dataset_bytes,context,chunks,tokens,ppl,ppl_stderr,duration_ms,cpu_ms,peak_memory,raw_json,created_at) "
              "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
              {id, resolvedModel, versionId, quantType, path, datasetPath, datasetKind,
               static_cast<int64_t>(datasetBytes), context, chunks,
               static_cast<int64_t>(chunks) * context, ppl, stderrValue,
               result.durationMs, cpuMs, peakMemory, raw.dump(), isoNow()});

    events_.publish(json{{"type", "ppl"}, {"status", "result"}, {"id", id}, {"quantType", quantType},
                         {"ppl", ppl}, {"path", path}});

    return json{{"id", id}, {"modelId", resolvedModel}, {"versionId", versionId},
                {"quantType", quantType}, {"path", path},
                {"datasetPath", datasetPath}, {"datasetKind", datasetKind},
                {"context", context}, {"chunks", chunks}, {"tokens", static_cast<int64_t>(chunks) * context},
                {"ppl", ppl}, {"pplStderr", stderrValue},
                {"durationMs", result.durationMs}, {"cpuMs", cpuMs}, {"peakMemory", peakMemory},
                {"finalLine", finalLine}, {"createdAt", isoNow()}};
}

void QualityService::workerMain(std::vector<json> targets, std::string datasetPath, std::string datasetKind,
                                int context, int threads, std::string modelId) {
    const std::shared_ptr<std::atomic<bool>> cancel = cancelFlag_;
    json collected = json::array();
    int completed = 0;
    int failed = 0;
    std::string lastErrorCode;
    std::string lastErrorMessage;

    for (const auto& target : targets) {
        if (cancel && cancel->load()) break;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_.currentTarget = target.value("path", "");
            job_.message = "正在评测 " + target.value("path", "");
            job_.progress = job_.total > 0 ? static_cast<int>(completed * 100 / job_.total) : 0;
        }
        std::string code, message;
        json item = evaluateOne(target, datasetPath, datasetKind, context, threads, modelId, cancel, code, message);
        if (item.is_null()) {
            ++failed;
            lastErrorCode = code;
            lastErrorMessage = message;
            collected.push_back(json{{"ok", false}, {"path", target.value("path", "")},
                                     {"errorCode", code}, {"errorMessage", message}});
            appendLog("ERROR", message);
        } else {
            ++completed;
            collected.push_back(item);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_.results = collected;
            job_.completed = completed;
            job_.progress = job_.total > 0
                ? std::min(99, static_cast<int>((completed + failed) * 100 / job_.total)) : 0;
        }
    }

    std::string publishStatus;
    int publishFailed = failed;
    {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    job_.finishedAt = nowMs();
    job_.results = collected;
    job_.completed = completed;
    if (cancel && cancel->load()) {
        job_.status = "cancelled";
        job_.message = "质量评测已取消";
        job_.errorCode = "CANCELLED";
    } else if (completed == 0 && failed > 0) {
        job_.status = "failed";
        job_.message = "质量评测失败";
        job_.errorCode = lastErrorCode;
        job_.errorMessage = lastErrorMessage;
    } else {
        job_.status = "completed";
        job_.progress = 100;
        job_.message = failed > 0
            ? "完成：" + std::to_string(completed) + " 成功，" + std::to_string(failed) + " 失败"
            : "完成：" + std::to_string(completed) + " 个文件";
    }
    publishStatus = job_.status;
    Logger::instance().info("[quality] " + job_.message);
    }
    events_.publish(json{{"type", "ppl"}, {"status", publishStatus},
                         {"completed", completed}, {"failed", publishFailed}});
}

json QualityService::comparison(const std::string& modelId) {
    const json model = system_.getModel(modelId);
    if (model.is_null()) return nullptr;

    json rows = json::array();
    json latestDataset = json::object();
    double bestPpl = 0.0;
    int withPpl = 0;

    const auto versions = db_.query(
        "SELECT * FROM model_versions WHERE model_id=? ORDER BY created_at ASC LIMIT 200", {modelId});
    for (const auto& version : versions) {
        const std::string vpath = version.value("path", "");
        const std::string quant = version.value("quant_type", "");
        json pplRow = json::object();
        const auto ppls = db_.query(
            "SELECT * FROM ppl_results WHERE gguf_path=? ORDER BY created_at DESC LIMIT 1", {pathKey(vpath)});
        if (!ppls.empty()) {
            const auto& row = ppls[0];
            pplRow = json{{"id", row.value("id", "")}, {"ppl", row.value("ppl", 0.0)},
                          {"pplStderr", row.value("ppl_stderr", 0.0)},
                          {"chunks", row.value("chunks", 0)}, {"tokens", row.value("tokens", 0)},
                          {"context", row.value("context", 0)},
                          {"durationMs", row.value("duration_ms", 0ll)},
                          {"cpuMs", row.value("cpu_ms", 0ll)},
                          {"peakMemory", row.value("peak_memory", 0ll)},
                          {"datasetKind", row.value("dataset_kind", "")},
                          {"datasetPath", row.value("dataset_path", "")},
                          {"createdAt", row.value("created_at", "")}};
            const double value = row.value("ppl", 0.0);
            if (value > 0) {
                bestPpl = bestPpl <= 0 ? value : std::min(bestPpl, value);
                ++withPpl;
            }
            if (latestDataset.empty()) {
                latestDataset = json{{"kind", row.value("dataset_kind", "")},
                                     {"path", row.value("dataset_path", "")},
                                     {"context", row.value("context", 0)}};
            }
        }
        json bench = json::object();
        if (!quant.empty()) {
            const auto benches = db_.query(
                "SELECT metrics_json FROM benchmark_results WHERE model_id=? AND quant_type=? "
                "ORDER BY created_at DESC LIMIT 1", {modelId, quant});
            if (!benches.empty()) {
                try {
                    const json metrics = json::parse(benches[0].value("metrics_json", "[]"));
                    if (metrics.is_array()) {
                        for (const auto& entry : metrics) {
                            if (!entry.is_object()) continue;
                            if (entry.contains("avg_ts")) {
                                const double ts = entry.value("avg_ts", 0.0);
                                if (ts <= 0) continue;
                                if (entry.value("n_prompt", 0) > 0) bench["promptTps"] = ts;
                                else if (entry.value("n_gen", 0) > 0) bench["genTps"] = ts;
                            }
                        }
                    }
                } catch (...) {}
            }
        }
        std::error_code ec;
        const bool exists = !vpath.empty() && fs::is_regular_file(pathFromUtf8(vpath), ec);
        rows.push_back(json{{"versionId", version.value("id", "")}, {"quantType", quant},
                            {"path", vpath}, {"size", version.value("size", 0ll)},
                            {"sha256", version.value("sha256", "")}, {"exists", exists},
                            {"createdAt", version.value("created_at", "")},
                            {"ppl", pplRow}, {"benchmark", bench}});
    }

    for (auto& row : rows) {
        const double value = row["ppl"].is_object() ? row["ppl"].value("ppl", 0.0) : 0.0;
        if (value > 0 && bestPpl > 0) {
            row["pplDelta"] = std::round((value - bestPpl) / bestPpl * 10000.0) / 100.0;
        }
    }

    return json{{"modelId", modelId}, {"modelName", model.value("name", "")},
                {"sourceSize", model.value("totalSize", 0ll)},
                {"rows", rows}, {"bestPpl", bestPpl}, {"scored", withPpl},
                {"dataset", latestDataset},
                {"caveat", "困惑度只反映模型对评测文本的确定性，不等于任务质量；不同文本、不同上下文的结果不可直接比较。"}};
}

} // namespace mf
