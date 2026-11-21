#pragma once

#include "ghc_filesystem.hpp"

#include <mutex>
#include <string>

namespace mf {

class Logger {
public:
    static Logger& instance();
    void setLogDirectory(const ghc::filesystem::path& directory);
    void log(const std::string& level, const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

private:
    Logger() = default;
    std::mutex mutex_;
    ghc::filesystem::path directory_;
};

} // namespace mf
