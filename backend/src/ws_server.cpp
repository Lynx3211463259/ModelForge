#include "modelforge/ws_server.hpp"

#include "modelforge/crypto.hpp"
#include "modelforge/util.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace mf {

namespace {
#ifdef _WIN32
void closeSocket(socket_t socket) { if (socket != kInvalidSocket) closesocket(socket); }
int lastSocketError() { return WSAGetLastError(); }
#else
void closeSocket(socket_t socket) { if (socket != kInvalidSocket) ::close(socket); }
int lastSocketError() { return errno; }
#endif

bool recvAll(socket_t socket, char* data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const int result = ::recv(socket, data + offset, static_cast<int>(length - offset), 0);
        if (result <= 0) return false;
        offset += static_cast<size_t>(result);
    }
    return true;
}

bool sendAll(socket_t socket, const char* data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        const int result = ::send(socket, data + offset, static_cast<int>(length - offset), 0);
        if (result <= 0) return false;
        offset += static_cast<size_t>(result);
    }
    return true;
}

std::vector<uint8_t> makeFrame(uint8_t opcode, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>(0x80 | opcode));
    const size_t size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<uint8_t>(size));
    } else if (size <= 0xffff) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((size >> 8) & 0xff));
        frame.push_back(static_cast<uint8_t>(size & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(size) >> (i * 8)) & 0xff));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}
} // namespace

struct WsServer::Impl {
    struct Client {
        socket_t socket = kInvalidSocket;
        std::mutex sendMutex;
    };

    socket_t listener = kInvalidSocket;
    std::atomic<bool> running{false};
    int boundPort = 0;
    std::thread acceptThread;
    std::mutex clientsMutex;
    std::set<std::shared_ptr<Client>> clients;

    bool sendFrame(const std::shared_ptr<Client>& client, uint8_t opcode, const std::string& payload) {
        auto frame = makeFrame(opcode, payload);
        std::lock_guard<std::mutex> lock(client->sendMutex);
        return sendAll(client->socket, reinterpret_cast<const char*>(frame.data()), frame.size());
    }

    void removeClient(const std::shared_ptr<Client>& client) {
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.erase(client);
        }
        closeSocket(client->socket);
        client->socket = kInvalidSocket;
    }

    bool handshake(socket_t socket) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const int result = ::recv(socket, buffer, sizeof(buffer), 0);
            if (result <= 0) return false;
            request.append(buffer, static_cast<size_t>(result));
            if (request.size() > 32768) return false;
        }
        std::string key;
        std::istringstream stream(request);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            if (toLower(trim(line.substr(0, colon))) == "sec-websocket-key") {
                key = trim(line.substr(colon + 1));
                break;
            }
        }
        if (key.empty()) return false;
        const std::string accept = base64Encode(sha1Raw(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        return sendAll(socket, response.data(), response.size());
    }

    void readLoop(std::shared_ptr<Client> client) {
        while (running.load()) {
            uint8_t header[2];
            if (!recvAll(client->socket, reinterpret_cast<char*>(header), 2)) break;
            const bool masked = (header[1] & 0x80) != 0;
            uint64_t length = header[1] & 0x7f;
            if (length == 126) {
                uint8_t ext[2];
                if (!recvAll(client->socket, reinterpret_cast<char*>(ext), 2)) break;
                length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
            } else if (length == 127) {
                uint8_t ext[8];
                if (!recvAll(client->socket, reinterpret_cast<char*>(ext), 8)) break;
                length = 0;
                for (int i = 0; i < 8; ++i) length = (length << 8) | ext[i];
            }
            if (length > 1024 * 1024) break;
            uint8_t mask[4]{};
            if (masked && !recvAll(client->socket, reinterpret_cast<char*>(mask), 4)) break;
            std::string payload(static_cast<size_t>(length), '\0');
            if (length && !recvAll(client->socket, &payload[0], static_cast<size_t>(length))) break;
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= static_cast<char>(mask[i % 4]);
            }
            const uint8_t opcode = header[0] & 0x0f;
            if (opcode == 0x8) break;
            if (opcode == 0x9) {
                if (!sendFrame(client, 0xA, payload)) break;
            }
        }
        removeClient(client);
    }

    void acceptLoop() {
        while (running.load()) {
            sockaddr_in address{};
            socklen_t length = sizeof(address);
            socket_t clientSocket = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &length);
            if (clientSocket == kInvalidSocket) {
                if (!running.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (!handshake(clientSocket)) {
                closeSocket(clientSocket);
                continue;
            }
            auto client = std::make_shared<Client>();
            client->socket = clientSocket;
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                clients.insert(client);
            }
            std::thread([this, client] { readLoop(client); }).detach();
        }
    }

    void stop() {
        running.store(false);
        if (listener != kInvalidSocket) {
            closeSocket(listener);
            listener = kInvalidSocket;
        }
        if (acceptThread.joinable()) acceptThread.join();
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto& client : clients) closeSocket(client->socket);
        clients.clear();
    }
};

WsServer::WsServer() : impl_(new Impl()) {}
WsServer::~WsServer() { stop(); }

bool WsServer::start(const std::string& host, int port, std::string& error) {
    if (impl_->running.load()) return true;
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        error = "WebSocket: WSAStartup 失败";
        return false;
    }
#endif
    impl_->listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listener == kInvalidSocket) {
        error = "WebSocket: 无法创建监听套接字";
        return false;
    }
    int reuse = 1;
    setsockopt(impl_->listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "localhost" || host == "127.0.0.1") {
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (::bind(impl_->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = "WebSocket: 端口 " + std::to_string(port) + " 绑定失败";
        closeSocket(impl_->listener);
        impl_->listener = kInvalidSocket;
        return false;
    }
    if (::listen(impl_->listener, 16) != 0) {
        error = "WebSocket: 监听失败";
        closeSocket(impl_->listener);
        impl_->listener = kInvalidSocket;
        return false;
    }
    sockaddr_in bound{};
    socklen_t boundLength = sizeof(bound);
    if (getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
        impl_->boundPort = ntohs(bound.sin_port);
    } else {
        impl_->boundPort = port;
    }
    impl_->running.store(true);
    impl_->acceptThread = std::thread([this] { impl_->acceptLoop(); });
    return true;
}

void WsServer::stop() { impl_->stop(); }

void WsServer::broadcast(const json& message) {
    if (!impl_->running.load()) return;
    const std::string payload = message.dump();
    std::vector<std::shared_ptr<Impl::Client>> snapshot;
    {
        std::lock_guard<std::mutex> lock(impl_->clientsMutex);
        snapshot.assign(impl_->clients.begin(), impl_->clients.end());
    }
    for (auto& client : snapshot) {
        if (!impl_->sendFrame(client, 0x1, payload)) impl_->removeClient(client);
    }
}

int WsServer::port() const { return impl_->boundPort; }
bool WsServer::running() const { return impl_->running.load(); }

} // namespace mf
