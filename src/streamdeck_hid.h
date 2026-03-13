#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "key_label_renderer.h"

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

struct StreamDeckKeyVisual {
    int key_index = -1;
    std::string label;
    bool has_binding = false;
    bool resolved = false;
    bool hold_mode = false;
    int max_text_scale = 0;
    bool has_background = false;
    RgbColor background;
    bool has_foreground = false;
    RgbColor foreground;
    bool has_accent = false;
    RgbColor accent;
};

enum class StreamDeckBackendLogLevel {
    Error,
    Warn,
    Info,
    Debug,
};

using StreamDeckKeyEventCallback = std::function<void(int keyIndex, bool pressed)>;
using StreamDeckBackendLogCallback = std::function<void(StreamDeckBackendLogLevel level, const std::string& message)>;

class StreamDeckHidBackend {
public:
    StreamDeckHidBackend();
    ~StreamDeckHidBackend();

    StreamDeckHidBackend(const StreamDeckHidBackend&) = delete;
    StreamDeckHidBackend& operator=(const StreamDeckHidBackend&) = delete;

    void setEventCallback(StreamDeckKeyEventCallback callback);
    void setLogCallback(StreamDeckBackendLogCallback callback);
    bool start(const std::string& preferredSerial, int brightnessPercent, std::string* errorMessage = nullptr);
    void stop();
    bool applyKeyVisuals(const std::vector<StreamDeckKeyVisual>& visuals, std::string* errorMessage = nullptr);

    StreamDeckInfo currentDeck() const;
    std::string statusLine() const;

private:
    bool openDeviceLocked(const std::string& preferredSerial, int brightnessPercent, std::string* errorMessage);
    bool applyKeyVisualsLocked(const std::vector<StreamDeckKeyVisual>& visuals, std::string* errorMessage);
    void closeDeviceLocked();
    void workerLoop();
    void setStatusLocked(const std::string& status);
    void emitLogLocked(StreamDeckBackendLogLevel level, const std::string& message) const;
    void emitLog(StreamDeckBackendLogLevel level, const std::string& message) const;

    std::string toUtf8Lossy(const wchar_t* value) const;
    std::string deviceErrorString(struct hid_device_* device) const;

    mutable std::mutex mutex_;
    struct hid_device_* device_ = nullptr;
    std::thread readerThread_;
    std::atomic<bool> stopRequested_{false};
    StreamDeckKeyEventCallback eventCallback_;
    StreamDeckBackendLogCallback logCallback_;
    StreamDeckInfo currentDeck_;
    std::vector<bool> lastKeyStates_;
    std::string statusLine_ = "Deck backend idle.";
};

} // namespace xpstreamdeck
