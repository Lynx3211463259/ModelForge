#pragma once

#include "httplib.h"
#include "modelforge/util.hpp"

#include <functional>
#include <mutex>
#include <string>

namespace mf {

// Records every route as it is registered so the generated OpenAPI document can
// never drift from the real server. Handlers are registered with httplib exactly
// as before; this wrapper only adds bookkeeping.
class OpenApiRegistry {
public:
    explicit OpenApiRegistry(httplib::Server& server) : server_(server) {}

    template <typename Handler>
    void Get(const std::string& path, Handler handler) {
        server_.Get(path, handler);
        record("get", path);
    }
    template <typename Handler>
    void Post(const std::string& path, Handler handler) {
        server_.Post(path, handler);
        record("post", path);
    }
    template <typename Handler>
    void Put(const std::string& path, Handler handler) {
        server_.Put(path, handler);
        record("put", path);
    }
    template <typename Handler>
    void Patch(const std::string& path, Handler handler) {
        server_.Patch(path, handler);
        record("patch", path);
    }
    template <typename Handler>
    void Delete(const std::string& path, Handler handler) {
        server_.Delete(path, handler);
        record("delete", path);
    }

    json routes() const;

    // Builds an OpenAPI 3.0 document from the recorded routes plus the per-route
    // metadata table. Routes without an entry are still emitted (summary derived
    // from the path) and flagged with "x-documented": false.
    static json build(const json& routes, const std::string& version);

    // "([^/]+)" style patterns become "{modelId}" style templates.
    static std::string normalizePath(const std::string& pattern);

private:
    void record(const std::string& method, const std::string& pattern);

    httplib::Server& server_;
    mutable std::mutex mutex_;
    json routes_ = json::array();
};

} // namespace mf
