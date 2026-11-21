#include "modelforge/logger.hpp"

#include "modelforge/util.hpp"

#include <fstream>
#include <iostream>

namespace mf {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLogDirectory(const ghc::filesystem::path& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    directory_ = directory;
    std::error_code ec;
    fs::create_directories(directory_, ec);
}

void Logger::log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string line = isoNow() + " [" + level + "] " + sanitizeSingleLine(message, 32000);
    std::cerr << line << std::endl;
    if (directory_.empty()) return;
    std::error_code ec;
    fs::create_directories(directory_, ec);
    fs::path file = directory_ / ("app-" + isoNow().substr(0, 10) + ".log");
    try {
        if (fs::exists(file, ec) && fs::file_size(file, ec) > 20ull * 1024 * 1024) {
            fs::path rotated = directory_ / ("app-" + isoNow().substr(0, 10) + "-" +
                std::to_string(static_cast<long long>(fs::file_size(file, ec))) + ".log");
            fs::rename(file, rotated, ec);
        }
    } catch (...) {}
    std::ofstream out(file.u8string(), std::ios::app | std::ios::binary);
    if (out) out << line << "\n";
}

void Logger::info(const std::string& message) { log("INFO", message); }
void Logger::warn(const std::string& message) { log("WARN", message); }
void Logger::error(const std::string& message) { log("ERROR", message); }

} // namespace mf
