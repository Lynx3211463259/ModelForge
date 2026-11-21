#pragma once

#include "json.hpp"
#include "sqlite3.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace mf {

using json = nlohmann::json;

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();
    bool exec(const std::string& sql, std::string& error);
    json query(const std::string& sql, const std::vector<json>& params = {}, std::string* error = nullptr);
    int changes();
    std::string lastError() const;

    void transaction(const std::function<void()>& fn);

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
    std::string path_;
    std::string lastError_;

    void migrate();
};

} // namespace mf
