#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct hid_device_;

namespace xpstreamdeck {

struct StreamDeckInfo {
    bool connected = false;
    std::string path;
    std::string serial;
    std::string product_name;
    unsigned short vendor_id = 0;
    unsigned short product_id = 0;
    std::size_t key_count = 0;
};

using StreamDeckKeyEventCallback = std::function<void(int keyIndex, bool pressed)>;

class StreamDeckHidBackend {
public:
    StreamDeckHidBackend();
    ~StreamDeckHidBackend();

    StreamDeckHidBackend(const StreamDeckHidBackend&) = delete;
    StreamDeckHidBackend& operator=(const StreamDeckHidBackend&) = delete;

    void setEventCallback(StreamDeckKeyEventCallback callback);
    bool start(const std::string& preferredSerial, int brightnessPercent, std::string* errorMessage = nullptr);
    void stop();

    StreamDeckInfo currentDeck() const;
    std::string statusLine() const;

private:
    bool openDeviceLocked(const std::string& preferredSerial, int brightnessPercent, std::string* errorMessage);
    void closeDeviceLocked();
    void workerLoop();
    void setStatusLocked(const std::string& status);

    std::string toUtf8Lossy(const wchar_t* value) const;
    std::string deviceErrorString(struct hid_device_* device) const;

    mutable std::mutex mutex_;
    struct hid_device_* device_ = nullptr;
    std::thread readerThread_;
    std::atomic<bool> stopRequested_{false};
    StreamDeckKeyEventCallback eventCallback_;
    StreamDeckInfo currentDeck_;
    std::vector<bool> lastKeyStates_;
    std::string statusLine_ = "Deck backend idle.";
};

} // namespace xpstreamdeck
