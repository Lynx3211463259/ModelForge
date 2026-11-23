#include "modelforge/fs_browser.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mf {
namespace fs_browse {

namespace {

constexpr size_t kMaxEntries = 2000;

std::string homePath() {
#ifdef _WIN32
    for (const char* name : {"USERPROFILE", "HOME"}) {
        const char* value = std::getenv(name);
        if (value && *value) return value;
    }
#else
    const char* value = std::getenv("HOME");
    if (value && *value) return value;
#endif
    return "";
}

std::string labelFor(const fs::path& path) {
    const std::string name = path.filename().u8string();
    return name.empty() ? pathToUtf8(path) : name;
}

} // namespace

json Listing::toJson() const {
    return json{{"path", path}, {"parent", parent}, {"entries", entries}, {"truncated", truncated}};
}

json roots() {
    json items = json::array();
#ifdef _WIN32
    const DWORD mask = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if ((mask & (1u << index)) == 0) continue;
        const char letter = static_cast<char>('A' + index);
        const std::string path = std::string(1, letter) + ":\\";
        std::error_code ec;
        if (!fs::is_directory(pathFromUtf8(path), ec)) continue;
        items.push_back(json{{"label", std::string(1, letter) + ":"}, {"path", path}, {"kind", "drive"}});
    }
#else
    items.push_back(json{{"label", "/"}, {"path", "/"}, {"kind", "root"}});
#endif
    const std::string home = homePath();
    if (!home.empty()) {
        std::error_code ec;
        if (fs::is_directory(pathFromUtf8(home), ec)) {
            items.push_back(json{{"label", labelFor(pathFromUtf8(home))}, {"path", pathToUtf8(pathFromUtf8(home))},
                                 {"kind", "home"}});
        }
    }
    return items;
}

std::string classifyError(const std::error_code& ec) {
    if (!ec) return "";
    if (ec == std::errc::permission_denied) return "DIRECTORY_PERMISSION_DENIED";
    if (ec == std::errc::no_such_file_or_directory) return "DIRECTORY_NOT_FOUND";
    if (ec == std::errc::not_a_directory) return "NOT_A_DIRECTORY";
    return "DIRECTORY_LIST_FAILED";
}

Listing list(const std::string& requestedPath) {
    Listing result;

    std::string input = trim(requestedPath);
    fs::path path;
    if (input.empty()) {
        input = homePath();
        if (input.empty()) input = pathToUtf8(fs::current_path());
    }
    path = pathFromUtf8(input);

    std::error_code ec;
    if (!path.is_absolute()) path = fs::absolute(path, ec);
    if (ec || !path.is_absolute()) {
        result.code = "INVALID_PATH";
        result.message = "目录路径必须是绝对路径。";
        result.suggestion = "请从根目录开始浏览，或直接输入完整路径。";
        return result;
    }
    path = fs::weakly_canonical(path, ec);
    if (ec) {
        result.code = classifyError(ec);
        result.message = "无法解析目录路径。";
        result.suggestion = "请检查路径拼写后重试。";
        return result;
    }

    result.path = pathToUtf8(path);
    result.parent = path.has_parent_path() && path.parent_path() != path ? pathToUtf8(path.parent_path()) : "";

    if (!fs::exists(path, ec)) {
        result.code = "DIRECTORY_NOT_FOUND";
        result.message = "目录不存在：" + result.path;
        result.suggestion = "请选择其他目录，或先创建该目录。";
        return result;
    }
    if (!fs::is_directory(path, ec)) {
        result.code = "NOT_A_DIRECTORY";
        result.message = "该路径不是目录：" + result.path;
        result.suggestion = "请选择一个文件夹。";
        return result;
    }

    fs::directory_iterator iterator(path, ec);
    if (ec) {
        result.code = classifyError(ec);
        result.message = "无法读取目录：" + result.path;
        result.suggestion = "请检查当前账户是否有访问权限，或返回上一级选择其他目录。";
        return result;
    }

    std::vector<json> directories;
    for (const auto& entry : iterator) {
        if (directories.size() >= kMaxEntries) {
            result.truncated = true;
            break;
        }
        std::error_code entryError;
        if (!entry.is_directory(entryError) || entryError) continue;
        const std::string name = entry.path().filename().u8string();
        if (name.empty()) continue;
        directories.push_back(json{{"name", name},
                                   {"path", pathToUtf8(entry.path())},
                                   {"hidden", name.front() == '.'}});
    }
    std::sort(directories.begin(), directories.end(), [](const json& left, const json& right) {
        return toLower(left.value("name", "")) < toLower(right.value("name", ""));
    });
    result.entries = json::array();
    for (auto& item : directories) result.entries.push_back(std::move(item));
    result.ok = true;
    return result;
}

} // namespace fs_browse
} // namespace mf
