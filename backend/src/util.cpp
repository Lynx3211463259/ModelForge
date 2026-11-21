#include "modelforge/util.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <ctime>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <lmcons.h>
#else
#include <unistd.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_host.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace mf {

namespace {
#ifdef _WIN32
std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], length);
    if (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], length, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}
#endif
} // namespace

std::string isoNow() {
    return isoFromMs(nowMs());
}

std::string isoFromMs(int64_t ms) {
    std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm tmValue{};
#ifdef _WIN32
    localtime_s(&tmValue, &seconds);
#else
    localtime_r(&seconds, &tmValue);
#endif
    std::ostringstream out;
    out << std::put_time(&tmValue, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << (ms % 1000);
    return out.str();
}

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string newId(const std::string& prefix) {
    static std::mutex mutex;
    static std::mt19937_64 rng([] {
        std::random_device rd;
        return (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ static_cast<uint64_t>(nowMs());
    }());
    uint64_t value = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        value = rng();
    }
    std::ostringstream out;
    out << prefix << '_' << std::hex << nowMs() << value % 0xffffff;
    return out.str();
}

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string toUpper(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream input(s);
    while (std::getline(input, current, delimiter)) parts.push_back(current);
    return parts;
}

std::string join(const std::vector<std::string>& values, const std::string& separator) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += separator;
        out += values[i];
    }
    return out;
}

std::string replaceAll(std::string value, const std::string& from, const std::string& to) {
    if (from.empty()) return value;
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::string readTextFile(const fs::path& path, uint64_t maxBytes) {
    std::ifstream input(path.u8string(), std::ios::binary);
    if (!input) throw std::runtime_error("无法读取文件: " + pathToUtf8(path));
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) throw std::runtime_error("无法获取文件大小: " + pathToUtf8(path));
    if (static_cast<uint64_t>(size) > maxBytes) throw std::runtime_error("文件过大，拒绝读取: " + pathToUtf8(path));
    input.seekg(0, std::ios::beg);
    std::string content(static_cast<size_t>(size), '\0');
    if (!content.empty()) input.read(&content[0], static_cast<std::streamsize>(content.size()));
    return content;
}

bool writeTextFile(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream output(path.u8string(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

bool writeTextFileAtomic(const fs::path& path, const std::string& content) {
    const fs::path temp = path.string() + ".tmp";
    if (!writeTextFile(temp, content)) return false;
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temp, path, ec);
    }
    return !ec;
}

json readJsonFile(const fs::path& path) {
    return json::parse(readTextFile(path));
}

std::string humanBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : (value >= 100 ? 1 : 2)) << value << ' ' << units[unit];
    return out.str();
}

std::string formatDuration(int64_t ms) {
    if (ms < 0) ms = 0;
    const int64_t totalSeconds = ms / 1000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds % 3600) / 60;
    const int64_t seconds = totalSeconds % 60;
    std::ostringstream out;
    if (hours) out << hours << "h ";
    if (hours || minutes) out << minutes << "m ";
    out << seconds << "s";
    return out.str();
}

std::string sanitizeFileName(const std::string& name) {
    std::string value = trim(name);
    if (value.empty()) value = "model";
    for (char& c : value) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
        if (static_cast<unsigned char>(c) < 32) c = '_';
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) value.pop_back();
    if (value.empty()) value = "model";
    if (value == "." || value == "..") value = "model";
    const std::string lower = toLower(value);
    static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "lpt1", "lpt2", "lpt3"};
    for (const char* word : reserved) {
        if (lower == word) value = "model_" + value;
    }
    if (value.size() > 120) value = value.substr(0, 120);
    return value;
}

std::string sanitizeSingleLine(const std::string& value, size_t maxLength) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\t') out.push_back(' ');
        else if (static_cast<unsigned char>(c) >= 32) out.push_back(c);
    }
    if (out.size() > maxLength) out.resize(maxLength);
    return out;
}

bool isPathInside(const fs::path& base, const fs::path& candidate) {
    auto baseAbs = fs::weakly_canonical(base);
    auto candidateAbs = fs::weakly_canonical(candidate);
    std::string b = baseAbs.generic_u8string();
    std::string c = candidateAbs.generic_u8string();
    if (b.empty() || c.empty()) return false;
    if (b.back() != '/') b.push_back('/');
    return c == baseAbs.generic_u8string() || c.compare(0, b.size(), b) == 0;
}

std::string pathToUtf8(const fs::path& path) {
    return path.u8string();
}

fs::path pathFromUtf8(const std::string& value) {
    return fs::u8path(value);
}

std::string shellQuote(const std::string& value, bool windows) {
    if (!windows) {
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out += "'";
        return out;
    }
    if (value.find_first_of(" \t\n\v\"") == std::string::npos) return value;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : value) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::string joinCommandForDisplay(const std::string& exe, const std::vector<std::string>& args, bool windows) {
    std::string out = shellQuote(exe, windows);
    for (const auto& arg : args) out += " " + shellQuote(arg, windows);
    return out;
}

std::string getExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return "";
    return wideToUtf8(std::wstring(buffer.data(), length));
#else
    std::vector<char> buffer(PATH_MAX + 1, '\0');
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return "";
    return std::string(buffer.data(), static_cast<size_t>(length));
#endif
}

std::string getHostName() {
#ifdef _WIN32
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buffer, &size)) return wideToUtf8(std::wstring(buffer, size));
    return "windows-host";
#else
    char buffer[256]{};
    if (gethostname(buffer, sizeof(buffer) - 1) == 0) return buffer;
    return "localhost";
#endif
}

std::string getOsName() {
#ifdef _WIN32
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

uint64_t totalSystemMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX state{};
    state.dwLength = sizeof(state);
    if (GlobalMemoryStatusEx(&state)) return static_cast<uint64_t>(state.ullTotalPhys);
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    return (pages > 0 && pageSize > 0) ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize) : 0;
#endif
}

uint64_t availableSystemMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX state{};
    state.dwLength = sizeof(state);
    if (GlobalMemoryStatusEx(&state)) return static_cast<uint64_t>(state.ullAvailPhys);
    return 0;
#elif defined(__APPLE__)
    // macOS has no _SC_AVPHYS_PAGES; free + inactive pages from vm_statistics64.
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t stats{};
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count) ==
        KERN_SUCCESS) {
        const long pageSize = sysconf(_SC_PAGE_SIZE);
        if (pageSize > 0) {
            return (static_cast<uint64_t>(stats.free_count) + static_cast<uint64_t>(stats.inactive_count)) *
                   static_cast<uint64_t>(pageSize);
        }
    }
    return totalSystemMemory();
#else
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long pageSize = sysconf(_SC_PAGE_SIZE);
    return (pages > 0 && pageSize > 0) ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize) : 0;
#endif
}

uint64_t diskFreeBytes(const fs::path& path) {
    std::error_code ec;
    const auto info = fs::space(path, ec);
    return ec ? 0 : static_cast<uint64_t>(info.available);
}

uint64_t diskTotalBytes(const fs::path& path) {
    std::error_code ec;
    const auto info = fs::space(path, ec);
    return ec ? 0 : static_cast<uint64_t>(info.capacity);
}

int defaultThreadCount() {
    const unsigned int count = std::thread::hardware_concurrency();
    return count ? static_cast<int>(count) : 4;
}

bool directoryWritable(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) return false;
    const fs::path probe = path / (".modelforge_write_test_" + newId("w"));
    {
        std::ofstream out(probe, std::ios::binary);
        if (!out) return false;
        out << "test";
    }
    fs::remove(probe, ec);
    return true;
}

bool ensureDirectory(const fs::path& path, std::string& error) {
    std::error_code ec;
    if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
        error = "路径已存在但不是目录: " + pathToUtf8(path);
        return false;
    }
    fs::create_directories(path, ec);
    if (ec) {
        error = "无法创建目录: " + pathToUtf8(path) + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

std::string mimeTypeForPath(const fs::path& path) {
    const std::string ext = toLower(path.extension().u8string());
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".map") return "application/json";
    return "application/octet-stream";
}

std::string urlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(value[i + 1]);
            int lo = hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (value[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(value[i]);
    }
    return out;
}

std::string base64UrlNoPadding(const std::string& value) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char c : value) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(buffer >> bits) & 0x3f]);
        }
    }
    if (bits > 0) out.push_back(alphabet[(buffer << (6 - bits)) & 0x3f]);
    return out;
}

std::string detectGpuSummary() {
#if defined(_WIN32)
    FILE* pipe = _popen("nvidia-smi --query-gpu=name,memory.total,memory.used --format=csv,noheader 2>NUL", "r");
#else
    FILE* pipe = popen("nvidia-smi --query-gpu=name,memory.total,memory.used --format=csv,noheader 2>/dev/null", "r");
#endif
    if (!pipe) return "未检测到 NVIDIA GPU";
    std::array<char, 512> buffer{};
    std::string output;
    if (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output = trim(buffer.data());
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output.empty() ? "未检测到 NVIDIA GPU" : output;
}

int checkTcpPort(int port, const std::string& host) {
    std::string addressText = host.empty() ? "127.0.0.1" : host;
    if (toLower(addressText) == "localhost") addressText = "127.0.0.1";
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, addressText.c_str(), &address.sin_addr) != 1) {
        closesocket(sock);
        WSACleanup();
        return -1;
    }
    address.sin_port = htons(static_cast<u_short>(port));
    const int bound = bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const int code = WSAGetLastError();
    closesocket(sock);
    WSACleanup();
    if (bound == 0) return 1;
    if (code == WSAEADDRINUSE || code == WSAEACCES) return 0;
    return -1;
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, addressText.c_str(), &address.sin_addr) != 1) {
        close(sock);
        return -1;
    }
    address.sin_port = htons(static_cast<uint16_t>(port));
    const int bound = bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    const int code = errno;
    close(sock);
    if (bound == 0) return 1;
    if (code == EADDRINUSE || code == EACCES) return 0;
    return -1;
#endif
}

} // namespace mf
