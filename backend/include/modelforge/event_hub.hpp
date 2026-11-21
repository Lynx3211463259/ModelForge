#pragma once

#include "json.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mf {

using json = nlohmann::json;

class EventHub {
public:
    using Observer = std::function<void(const json&)>;

    uint64_t subscribe();
    void unsubscribe(uint64_t id);
    std::vector<json> waitForEvents(uint64_t id, int timeoutMs);
    void publish(json event);
    void setObserver(Observer observer);
    json recent(int limit = 100) const;

private:
    struct Subscriber {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<json> queue;
    };
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Subscriber>> subscribers_;
    std::deque<json> recent_;
    uint64_t nextId_ = 1;
    Observer observer_;
};

} // namespace mf
