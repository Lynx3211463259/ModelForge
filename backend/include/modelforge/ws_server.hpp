#pragma once

#include "json.hpp"

#include <memory>
#include <string>

namespace mf {

using json = nlohmann::json;

class WsServer {
public:
    WsServer();
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    bool start(const std::string& host, int port, std::string& error);
    void stop();
    void broadcast(const json& message);
    int port() const;
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mf
