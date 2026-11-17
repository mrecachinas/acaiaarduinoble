#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace host {
using Rpc = int (*)(const char*, const char*, void*, unsigned int);
using Log = void (*)(const char*, uint64_t, const char*);
inline Rpc rpc = nullptr;
inline Log log = nullptr;
inline std::atomic<bool> stopping{false}, paddle{false};
inline std::atomic<int> requestedGoal{-1};
inline auto epoch = std::chrono::steady_clock::now();
inline int inputPin = -1, outputPin = -1;
inline int pins[64] = {};
inline std::mutex stateMutex;
inline std::string state = "{}";
struct Stop {};

inline unsigned long millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - epoch).count();
}
inline void emit(const char* kind, const std::string& text) {
    if (log) log(kind, millis(), text.c_str());
}
inline void checkStop() { if (stopping) throw Stop{}; }
struct Result { bool ok; std::string text; };
inline Result call(const char* operation, const std::string& argument = "") {
    checkStop();
    char buffer[65536] = {};
    if (!rpc) throw std::runtime_error("Native RPC callback is not installed");
    int length = rpc(operation, argument.c_str(), buffer, sizeof(buffer));
    checkStop();
    if (length < 0) {
        emit("transport_error", buffer);
        return {false, buffer};
    }
    if (length >= static_cast<int>(sizeof(buffer))) {
        throw std::runtime_error("Oversized native RPC response");
    }
    return {true, std::string(buffer, length)};
}
inline std::string hex(const uint8_t* bytes, size_t count) {
    const char* alphabet = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < count; ++i) {
        text += alphabet[bytes[i] >> 4];
        text += alphabet[bytes[i] & 15];
    }
    return text;
}
inline std::vector<uint8_t> unhex(const std::string& text) {
    if (text.size() % 2) throw std::runtime_error("Odd-length hex response");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < text.size(); i += 2) {
        auto digit = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            throw std::runtime_error("Invalid hex response");
        };
        bytes.push_back((digit(text[i]) << 4) | digit(text[i + 1]));
    }
    return bytes;
}
inline std::string unhexText(const std::string& text) {
    auto bytes = unhex(text);
    return std::string(bytes.begin(), bytes.end());
}
inline std::vector<std::string> split(const std::string& text, char delimiter = '\t') {
    std::vector<std::string> items;
    std::istringstream input(text);
    for (std::string item; std::getline(input, item, delimiter);) items.push_back(item);
    return items;
}
inline std::string uuid(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (value.size() == 4) value = "0000" + value;
    if (value.size() == 8) value += "-0000-1000-8000-00805f9b34fb";
    return value;
}
struct Notification { std::string uuid; std::vector<uint8_t> bytes; };
inline std::mutex notificationMutex;
inline std::deque<Notification> notifications;
inline std::function<void(const Notification&)> deliver;
inline void poll() {
    checkStop();
    std::deque<Notification> ready;
    {
        std::lock_guard<std::mutex> guard(notificationMutex);
        ready.swap(notifications);
    }
    for (const auto& notification : ready) {
        if (deliver) deliver(notification);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}
