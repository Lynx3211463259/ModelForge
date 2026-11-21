#include "modelforge/gguf_reader.hpp"

#include <cstring>
#include <fstream>
#include <map>

namespace mf {

namespace {
class BinaryReader {
public:
    explicit BinaryReader(const fs::path& path) : input_(path.u8string(), std::ios::binary) {
        input_.seekg(0, std::ios::end);
        size_ = static_cast<uint64_t>(input_.tellg());
        input_.seekg(0, std::ios::beg);
    }
    bool ok() const { return static_cast<bool>(input_); }
    uint64_t size() const { return size_; }
    bool read(void* data, size_t length) {
        if (position_ + length > size_) return false;
        input_.read(static_cast<char*>(data), static_cast<std::streamsize>(length));
        if (!input_) return false;
        position_ += length;
        return true;
    }
    template <typename T>
    bool readNumber(T& value) { return read(&value, sizeof(T)); }
    bool readString(std::string& value) {
        uint64_t length = 0;
        if (!readNumber(length)) return false;
        if (length > 16ull * 1024 * 1024 || position_ + length > size_) return false;
        value.assign(static_cast<size_t>(length), '\0');
        return length == 0 || read(&value[0], static_cast<size_t>(length));
    }
private:
    std::ifstream input_;
    uint64_t size_ = 0;
    uint64_t position_ = 0;
};

json readGgufValue(BinaryReader& reader, uint32_t type, int depth, std::string& error);

json readGgufArray(BinaryReader& reader, int depth, std::string& error) {
    uint32_t elementType = 0;
    uint64_t count = 0;
    if (!reader.readNumber(elementType) || !reader.readNumber(count)) {
        error = "GGUF 数组头读取失败";
        return nullptr;
    }
    if (count > 100000000ull) {
        error = "GGUF 数组长度异常";
        return nullptr;
    }
    const uint64_t previewLimit = 24;
    json preview = json::array();
    for (uint64_t i = 0; i < count; ++i) {
        json value = readGgufValue(reader, elementType, depth + 1, error);
        if (!error.empty()) return nullptr;
        if (i < previewLimit) preview.push_back(value);
    }
    return json{{"__array", true}, {"count", count}, {"preview", preview}};
}

json readGgufValue(BinaryReader& reader, uint32_t type, int depth, std::string& error) {
    if (depth > 3) { error = "GGUF 元数据嵌套过深"; return nullptr; }
    switch (type) {
        case 0: { uint8_t v; if (!reader.readNumber(v)) break; return v; }
        case 1: { int8_t v; if (!reader.readNumber(v)) break; return v; }
        case 2: { uint16_t v; if (!reader.readNumber(v)) break; return v; }
        case 3: { int16_t v; if (!reader.readNumber(v)) break; return v; }
        case 4: { uint32_t v; if (!reader.readNumber(v)) break; return v; }
        case 5: { int32_t v; if (!reader.readNumber(v)) break; return v; }
        case 6: { float v; if (!reader.readNumber(v)) break; return v; }
        case 7: { uint8_t v; if (!reader.readNumber(v)) break; return v != 0; }
        case 8: { std::string v; if (!reader.readString(v)) break; return v; }
        case 9: return readGgufArray(reader, depth, error);
        case 10: { uint64_t v; if (!reader.readNumber(v)) break; return v; }
        case 11: { int64_t v; if (!reader.readNumber(v)) break; return v; }
        case 12: { double v; if (!reader.readNumber(v)) break; return v; }
        default: error = "未知 GGUF 元数据类型: " + std::to_string(type); return nullptr;
    }
    error = "GGUF 元数据值读取失败";
    return nullptr;
}
} // namespace

std::string GgufReader::quantName(int fileType) {
    static const std::map<int, std::string> names = {
        {0, "F32"}, {1, "F16"}, {2, "Q4_0"}, {3, "Q4_1"}, {7, "Q8_0"}, {8, "Q5_0"}, {9, "Q5_1"},
        {10, "Q2_K"}, {11, "Q3_K_S"}, {12, "Q3_K_M"}, {13, "Q3_K_L"}, {14, "Q4_K_S"}, {15, "Q4_K_M"},
        {16, "Q5_K_S"}, {17, "Q5_K_M"}, {18, "Q6_K"}, {19, "IQ2_XXS"}, {20, "IQ2_XS"}, {21, "Q2_K_S"},
        {22, "IQ3_XS"}, {23, "IQ3_XXS"}, {24, "IQ1_S"}, {25, "IQ4_NL"}, {26, "IQ3_S"}, {27, "IQ3_M"},
        {28, "IQ2_S"}, {29, "IQ2_M"}, {30, "IQ4_XS"}, {31, "IQ1_M"}, {32, "BF16"}, {36, "TQ1_0"},
        {37, "TQ2_0"}, {38, "MXFP4_MOE"}, {39, "NVFP4"}, {40, "Q1_0"}
    };
    auto it = names.find(fileType);
    return it == names.end() ? ("FTYPE_" + std::to_string(fileType)) : it->second;
}

GgufInfo GgufReader::read(const fs::path& path, bool includeTensorNames) {
    GgufInfo info;
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
        info.errors.push_back("GGUF 文件不存在");
        return info;
    }
    info.fileSize = static_cast<uint64_t>(fs::file_size(path, ec));
    BinaryReader reader(path);
    if (!reader.ok()) {
        info.errors.push_back("无法打开 GGUF 文件");
        return info;
    }
    uint32_t magic = 0;
    if (!reader.readNumber(magic) || magic != 0x46554747) {
        info.errors.push_back("GGUF 魔数不匹配，文件可能损坏或格式不正确");
        return info;
    }
    if (!reader.readNumber(info.version) || !reader.readNumber(info.tensorCount) || !reader.readNumber(info.metadataCount)) {
        info.errors.push_back("GGUF 文件头不完整");
        return info;
    }
    if (info.version < 2 || info.version > 4) {
        info.errors.push_back("当前阅读器不支持 GGUF 版本 " + std::to_string(info.version));
        return info;
    }
    if (info.metadataCount > 1000000 || info.tensorCount > 10000000) {
        info.errors.push_back("GGUF 头部计数异常");
        return info;
    }
    for (uint64_t i = 0; i < info.metadataCount; ++i) {
        std::string key;
        uint32_t type = 0;
        if (!reader.readString(key) || !reader.readNumber(type)) {
            info.errors.push_back("GGUF 元数据读取中断");
            break;
        }
        std::string error;
        json value = readGgufValue(reader, type, 0, error);
        if (!error.empty()) {
            info.errors.push_back("字段 " + key + ": " + error);
            break;
        }
        info.metadata[key] = value;
    }
    if (!info.errors.empty()) return info;

    for (uint64_t i = 0; i < info.tensorCount; ++i) {
        std::string name;
        uint32_t dimensions = 0;
        if (!reader.readString(name) || !reader.readNumber(dimensions)) {
            info.errors.push_back("GGUF 张量信息读取中断");
            return info;
        }
        if (dimensions > 8) { info.errors.push_back("GGUF 张量维度异常"); return info; }
        std::vector<uint64_t> shape(dimensions);
        for (auto& d : shape) if (!reader.readNumber(d)) { info.errors.push_back("GGUF 张量维度读取失败"); return info; }
        uint32_t type = 0;
        uint64_t offset = 0;
        if (!reader.readNumber(type) || !reader.readNumber(offset)) {
            info.errors.push_back("GGUF 张量信息读取失败");
            return info;
        }
        if (includeTensorNames && i < 500) {
            info.tensors.push_back(json{{"name", name}, {"shape", shape}, {"type", type}, {"offset", offset}});
        }
    }
    info.architecture = info.metadata.value("general.architecture", std::string(""));
    info.modelName = info.metadata.value("general.name", std::string(""));
    if (info.metadata.contains("general.file_type") && info.metadata["general.file_type"].is_number_integer()) {
        info.quantType = quantName(info.metadata["general.file_type"].get<int>());
    }
    info.valid = info.errors.empty() && info.tensorCount > 0;
    if (!info.valid && info.errors.empty()) info.errors.push_back("GGUF 中没有张量");
    return info;
}

json GgufInfo::toJson() const {
    return json{
        {"valid", valid}, {"version", version}, {"tensorCount", tensorCount}, {"metadataCount", metadataCount},
        {"fileSize", fileSize}, {"architecture", architecture}, {"modelName", modelName}, {"quantType", quantType},
        {"metadata", metadata}, {"tensors", tensors}, {"errors", errors}
    };
}

} // namespace mf
