#include "modelforge/event_hub.hpp"

#include <chrono>

namespace mf {

uint64_t EventHub::subscribe() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t id = nextId_++;
    subscribers_[id] = std::make_shared<Subscriber>();
    return id;
}

void EventHub::unsubscribe(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.erase(id);
}

std::vector<json> EventHub::waitForEvents(uint64_t id, int timeoutMs) {
    std::shared_ptr<Subscriber> sub;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(id);
        if (it == subscribers_.end()) return {};
        sub = it->second;
    }
    std::unique_lock<std::mutex> lock(sub->mutex);
    if (sub->queue.empty()) {
        sub->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !sub->queue.empty(); });
    }
    std::vector<json> out;
    while (!sub->queue.empty()) {
        out.push_back(std::move(sub->queue.front()));
        sub->queue.pop_front();
    }
    return out;
}

void EventHub::publish(json event) {
    if (!event.contains("timestamp")) event["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    Observer observer;
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recent_.push_back(event);
        while (recent_.size() > 200) recent_.pop_front();
        for (auto& pair : subscribers_) subscribers.push_back(pair.second);
        observer = observer_;
    }
    for (auto& sub : subscribers) {
        {
            std::lock_guard<std::mutex> lock(sub->mutex);
            if (sub->queue.size() >= 500) sub->queue.pop_front();
            sub->queue.push_back(event);
        }
        sub->cv.notify_one();
    }
    if (observer) {
        try { observer(event); } catch (...) {}
    }
}

void EventHub::setObserver(Observer observer) {
    std::lock_guard<std::mutex> lock(mutex_);
    observer_ = std::move(observer);
}

json EventHub::recent(int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json array = json::array();
    const int start = limit > 0 && static_cast<int>(recent_.size()) > limit
        ? static_cast<int>(recent_.size()) - limit : 0;
    for (int i = start; i < static_cast<int>(recent_.size()); ++i) array.push_back(recent_[static_cast<size_t>(i)]);
    return array;
}

} // namespace mf
