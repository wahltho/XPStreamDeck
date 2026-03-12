#include "streamdeck_hid.h"

#include <hidapi.h>

#include "key_label_renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace xpstreamdeck {

namespace {

constexpr unsigned short kElgatoVendorId = 0x0fd9;
constexpr std::size_t kInputReportHeaderLength = 4;
constexpr std::size_t kFeatureReportLength = 32;
constexpr std::size_t kImageResetReportLength = 1024;
constexpr std::size_t kKeyImageReportLength = 1024;
constexpr std::size_t kKeyImageHeaderLength = 8;
constexpr std::size_t kKeyImageChunkLength = kKeyImageReportLength - kKeyImageHeaderLength;
constexpr int kKeyImageWidth = 72;
constexpr int kKeyImageHeight = 72;
constexpr int kReadTimeoutMs = 50;

struct SupportedProduct {
    unsigned short product_id = 0;
    const char* product_name = "";
    std::size_t key_count = 0;
};

constexpr std::array<SupportedProduct, 4> kSupportedProducts = {{
    {0x006d, "Stream Deck Original V2", 15},
    {0x0080, "Stream Deck MK.2", 15},
    {0x00a5, "Stream Deck MK.2 Scissor", 15},
    {0x00b9, "Stream Deck MK.2 Module", 15},
}};

const SupportedProduct* findSupportedProduct(unsigned short productId) {
    for (const auto& product : kSupportedProducts) {
        if (product.product_id == productId) {
            return &product;
        }
    }
    return nullptr;
}

std::string hex16(unsigned short value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

bool resetKeyStream(hid_device* device, std::string* errorMessage) {
    std::vector<unsigned char> payload(kImageResetReportLength, 0);
    payload[0] = 0x02;
    if (hid_write(device, payload.data(), payload.size()) < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to reset key image stream.";
        }
        return false;
    }
    return true;
}

bool setBrightness(hid_device* device, int brightnessPercent, std::string* errorMessage) {
    brightnessPercent = std::clamp(brightnessPercent, 0, 100);
    unsigned char payload[kFeatureReportLength] = {};
    payload[0] = 0x03;
    payload[1] = 0x08;
    payload[2] = static_cast<unsigned char>(brightnessPercent);
    if (hid_send_feature_report(device, payload, sizeof(payload)) < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to send brightness feature report.";
        }
        return false;
    }
    return true;
}

RgbColor backgroundForVisual(const StreamDeckKeyVisual& visual) {
    if (!visual.has_binding) {
        return {44, 48, 54};
    }
    if (!visual.resolved) {
        return {112, 34, 34};
    }
    if (visual.hold_mode) {
        return {133, 78, 24};
    }
    return {25, 61, 117};
}

bool writeKeyImage(hid_device* device, int keyIndex, const std::vector<unsigned char>& jpeg, std::string* errorMessage) {
    std::size_t offset = 0;
    std::uint16_t chunkIndex = 0;
    while (offset < jpeg.size() || (jpeg.empty() && chunkIndex == 0)) {
        const std::size_t chunkSize = std::min(kKeyImageChunkLength, jpeg.size() - offset);
        std::vector<unsigned char> report(kKeyImageReportLength, 0);
        report[0] = 0x02;
        report[1] = 0x07;
        report[2] = static_cast<unsigned char>(keyIndex);
        report[3] = static_cast<unsigned char>((offset + chunkSize) >= jpeg.size() ? 1 : 0);
        report[4] = static_cast<unsigned char>(chunkSize & 0xff);
        report[5] = static_cast<unsigned char>((chunkSize >> 8) & 0xff);
        report[6] = static_cast<unsigned char>(chunkIndex & 0xff);
        report[7] = static_cast<unsigned char>((chunkIndex >> 8) & 0xff);
        if (chunkSize > 0) {
            std::copy(jpeg.begin() + static_cast<std::ptrdiff_t>(offset), jpeg.begin() + static_cast<std::ptrdiff_t>(offset + chunkSize), report.begin() + static_cast<std::ptrdiff_t>(kKeyImageHeaderLength));
        }

        const int bytesWritten = hid_write(device, report.data(), report.size());
        if (bytesWritten < 0) {
            if (errorMessage != nullptr) {
                *errorMessage = "Failed to write key image report.";
            }
            return false;
        }

        offset += chunkSize;
        ++chunkIndex;
        if (jpeg.empty()) {
            break;
        }
    }
    return true;
}

} // namespace

StreamDeckHidBackend::StreamDeckHidBackend() {
    hid_init();
}

StreamDeckHidBackend::~StreamDeckHidBackend() {
    stop();
    hid_exit();
}

void StreamDeckHidBackend::setEventCallback(StreamDeckKeyEventCallback callback) {
    std::lock_guard<std::mutex> guard(mutex_);
    eventCallback_ = std::move(callback);
}

void StreamDeckHidBackend::setLogCallback(StreamDeckBackendLogCallback callback) {
    std::lock_guard<std::mutex> guard(mutex_);
    logCallback_ = std::move(callback);
}

bool StreamDeckHidBackend::start(const std::string& preferredSerial, int brightnessPercent, std::string* errorMessage) {
    emitLog(StreamDeckBackendLogLevel::Info,
        "Start requested. preferred_serial=" + (preferredSerial.empty() ? std::string("<auto>") : preferredSerial) +
        " brightness=" + std::to_string(std::clamp(brightnessPercent, 0, 100)) + "%");
    stop();

    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!openDeviceLocked(preferredSerial, brightnessPercent, errorMessage)) {
            if (errorMessage != nullptr && !errorMessage->empty()) {
                emitLogLocked(StreamDeckBackendLogLevel::Warn, "Start failed: " + *errorMessage);
            }
            return false;
        }
        stopRequested_.store(false);
    }

    readerThread_ = std::thread(&StreamDeckHidBackend::workerLoop, this);
    emitLog(StreamDeckBackendLogLevel::Debug, "Reader thread started.");
    return true;
}

void StreamDeckHidBackend::stop() {
    emitLog(StreamDeckBackendLogLevel::Debug, "Stop requested.");
    stopRequested_.store(true);

    if (readerThread_.joinable()) {
        emitLog(StreamDeckBackendLogLevel::Debug, "Joining reader thread.");
        readerThread_.join();
    }

    std::lock_guard<std::mutex> guard(mutex_);
    if (currentDeck_.connected) {
        emitLogLocked(StreamDeckBackendLogLevel::Info,
            "Closing device " + currentDeck_.product_name +
            (currentDeck_.serial.empty() ? std::string() : (" [" + currentDeck_.serial + "]")));
    }
    closeDeviceLocked();
    if (!currentDeck_.connected) {
        statusLine_ = "Deck backend idle.";
    }
}

bool StreamDeckHidBackend::applyKeyVisuals(const std::vector<StreamDeckKeyVisual>& visuals, std::string* errorMessage) {
    std::lock_guard<std::mutex> guard(mutex_);
    emitLogLocked(StreamDeckBackendLogLevel::Debug,
        "Applying " + std::to_string(visuals.size()) + " key visual(s).");
    return applyKeyVisualsLocked(visuals, errorMessage);
}

StreamDeckInfo StreamDeckHidBackend::currentDeck() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return currentDeck_;
}

std::string StreamDeckHidBackend::statusLine() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return statusLine_;
}

bool StreamDeckHidBackend::openDeviceLocked(const std::string& preferredSerial, int brightnessPercent, std::string* errorMessage) {
    struct SelectedDevice {
        std::string path;
        std::string serial;
        std::string product_name;
        unsigned short vendor_id = 0;
        unsigned short product_id = 0;
        std::size_t key_count = 0;
    };

    SelectedDevice selected;
    struct hid_device_info* devices = hid_enumerate(kElgatoVendorId, 0);
    for (struct hid_device_info* current = devices; current != nullptr; current = current->next) {
        const SupportedProduct* supported = findSupportedProduct(current->product_id);
        if (supported == nullptr) {
            emitLogLocked(StreamDeckBackendLogLevel::Debug,
                "Ignoring unsupported Elgato product pid=" + hex16(current->product_id));
            continue;
        }

        const std::string serial = toUtf8Lossy(current->serial_number);
        emitLogLocked(StreamDeckBackendLogLevel::Debug,
            "Found candidate product=" +
                std::string(current->product_string ? toUtf8Lossy(current->product_string) : supported->product_name) +
                " serial=" + (serial.empty() ? std::string("<none>") : serial) +
                " pid=" + hex16(current->product_id) +
                " path=" + std::string(current->path ? current->path : "<null>"));
        if (!preferredSerial.empty() && serial != preferredSerial) {
            emitLogLocked(StreamDeckBackendLogLevel::Debug,
                "Skipping candidate due to serial mismatch. wanted=" + preferredSerial + " found=" + serial);
            continue;
        }

        selected.path = current->path ? current->path : "";
        selected.serial = serial;
        selected.product_name = current->product_string ? toUtf8Lossy(current->product_string) : supported->product_name;
        selected.vendor_id = current->vendor_id;
        selected.product_id = current->product_id;
        selected.key_count = supported->key_count;
        break;
    }
    hid_free_enumeration(devices);

    if (selected.path.empty()) {
        const std::string message = preferredSerial.empty()
            ? "No supported Stream Deck MK.2-family device detected."
            : "Configured Stream Deck serial not found.";
        setStatusLocked(message);
        emitLogLocked(StreamDeckBackendLogLevel::Warn, message);
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    }

    emitLogLocked(StreamDeckBackendLogLevel::Info,
        "Opening device product=" + selected.product_name +
        (selected.serial.empty() ? std::string() : (" [" + selected.serial + "]")) +
        " pid=" + hex16(selected.product_id) +
        " path=" + selected.path);
    device_ = hid_open_path(selected.path.c_str());
    if (device_ == nullptr) {
        const std::string message = "Failed to open Stream Deck: " + selected.path;
        setStatusLocked(message);
        emitLogLocked(StreamDeckBackendLogLevel::Error, message);
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    }

    std::string resetError;
    if (!resetKeyStream(device_, &resetError)) {
        emitLogLocked(StreamDeckBackendLogLevel::Warn,
            resetError + " " + deviceErrorString(device_));
    } else {
        emitLogLocked(StreamDeckBackendLogLevel::Debug, "Key image stream reset.");
    }

    std::string brightnessError;
    if (!setBrightness(device_, brightnessPercent, &brightnessError)) {
        emitLogLocked(StreamDeckBackendLogLevel::Warn,
            brightnessError + " " + deviceErrorString(device_));
    } else {
        emitLogLocked(StreamDeckBackendLogLevel::Debug,
            "Brightness set to " + std::to_string(std::clamp(brightnessPercent, 0, 100)) + "%.");
    }

    currentDeck_.connected = true;
    currentDeck_.path = selected.path;
    currentDeck_.serial = selected.serial;
    currentDeck_.product_name = selected.product_name;
    currentDeck_.vendor_id = selected.vendor_id;
    currentDeck_.product_id = selected.product_id;
    currentDeck_.key_count = selected.key_count;
    lastKeyStates_.assign(currentDeck_.key_count, false);

    std::ostringstream status;
    status << "Connected: " << currentDeck_.product_name;
    if (!currentDeck_.serial.empty()) {
        status << " [" << currentDeck_.serial << "]";
    }
    setStatusLocked(status.str());
    emitLogLocked(StreamDeckBackendLogLevel::Info,
        "Device connected with " + std::to_string(currentDeck_.key_count) + " key(s).");
    return true;
}

bool StreamDeckHidBackend::applyKeyVisualsLocked(const std::vector<StreamDeckKeyVisual>& visuals, std::string* errorMessage) {
    if (device_ == nullptr || !currentDeck_.connected || currentDeck_.key_count == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "No Stream Deck connected for key image upload.";
        }
        return false;
    }

    std::unordered_map<int, StreamDeckKeyVisual> visualByKey;
    for (const auto& visual : visuals) {
        if (visual.key_index < 0) {
            continue;
        }
        visualByKey[visual.key_index] = visual;
    }

    for (std::size_t index = 0; index < currentDeck_.key_count; ++index) {
        StreamDeckKeyVisual visual;
        visual.key_index = static_cast<int>(index);
        if (const auto it = visualByKey.find(static_cast<int>(index)); it != visualByKey.end()) {
            visual = it->second;
        }

        emitLogLocked(StreamDeckBackendLogLevel::Debug,
            "Uploading key image for key." + std::to_string(index) +
            " label='" + visual.label + "'" +
            " bound=" + std::string(visual.has_binding ? "1" : "0") +
            " resolved=" + std::string(visual.resolved ? "1" : "0") +
            " hold=" + std::string(visual.hold_mode ? "1" : "0"));

        const auto background = backgroundForVisual(visual);
        const auto jpeg = renderLabelKeyJpeg(
            visual.label,
            kKeyImageWidth,
            kKeyImageHeight,
            background,
            {245, 247, 250});

        std::string writeError;
        if (!writeKeyImage(device_, static_cast<int>(index), jpeg, &writeError)) {
            const std::string message = writeError.empty()
                ? "Failed to upload key image."
                : (writeError + " " + deviceErrorString(device_));
            setStatusLocked(message);
            emitLogLocked(StreamDeckBackendLogLevel::Error, message);
            if (errorMessage != nullptr) {
                *errorMessage = message;
            }
            return false;
        }
    }

    emitLogLocked(StreamDeckBackendLogLevel::Debug, "Key image upload complete.");
    return true;
}

void StreamDeckHidBackend::closeDeviceLocked() {
    if (device_ != nullptr) {
        hid_close(device_);
        device_ = nullptr;
    }
    currentDeck_ = StreamDeckInfo{};
    lastKeyStates_.clear();
}

void StreamDeckHidBackend::workerLoop() {
    emitLog(StreamDeckBackendLogLevel::Debug, "Reader loop entered.");
    while (!stopRequested_.load()) {
        std::vector<unsigned char> report;
        std::vector<std::pair<int, bool>> changedKeys;
        StreamDeckKeyEventCallback callback;

        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (device_ == nullptr || currentDeck_.key_count == 0) {
                break;
            }

            report.assign(kInputReportHeaderLength + currentDeck_.key_count, 0);
            const int bytesRead = hid_read_timeout(device_, report.data(), report.size(), kReadTimeoutMs);
            if (bytesRead < 0) {
                const std::string error = deviceErrorString(device_);
                emitLogLocked(StreamDeckBackendLogLevel::Error,
                    error.empty() ? "Deck disconnected or read failed." : ("Deck read failed: " + error));
                closeDeviceLocked();
                setStatusLocked(error.empty() ? "Deck disconnected or read failed." : ("Deck read failed: " + error));
                break;
            }

            if (bytesRead == 0 || static_cast<std::size_t>(bytesRead) < (kInputReportHeaderLength + currentDeck_.key_count)) {
                continue;
            }

            callback = eventCallback_;
            for (std::size_t index = 0; index < currentDeck_.key_count; ++index) {
                const bool pressed = report[kInputReportHeaderLength + index] != 0;
                if (index >= lastKeyStates_.size() || lastKeyStates_[index] == pressed) {
                    continue;
                }
                lastKeyStates_[index] = pressed;
                emitLogLocked(StreamDeckBackendLogLevel::Debug,
                    "Key state changed key." + std::to_string(index) + "=" + (pressed ? std::string("pressed") : "released"));
                changedKeys.emplace_back(static_cast<int>(index), pressed);
            }
        }

        if (!callback) {
            continue;
        }

        for (const auto& [keyIndex, pressed] : changedKeys) {
            callback(keyIndex, pressed);
        }
    }
    emitLog(StreamDeckBackendLogLevel::Debug, "Reader loop exited.");
}

void StreamDeckHidBackend::setStatusLocked(const std::string& status) {
    statusLine_ = status;
}

void StreamDeckHidBackend::emitLogLocked(StreamDeckBackendLogLevel level, const std::string& message) const {
    if (logCallback_) {
        logCallback_(level, message);
    }
}

void StreamDeckHidBackend::emitLog(StreamDeckBackendLogLevel level, const std::string& message) const {
    std::lock_guard<std::mutex> guard(mutex_);
    emitLogLocked(level, message);
}

std::string StreamDeckHidBackend::toUtf8Lossy(const wchar_t* value) const {
    if (value == nullptr) {
        return {};
    }

    std::string out;
    while (*value != 0) {
        const wchar_t ch = *value++;
        out.push_back((ch >= 0 && ch <= 0x7f) ? static_cast<char>(ch) : '?');
    }
    return out;
}

std::string StreamDeckHidBackend::deviceErrorString(struct hid_device_* device) const {
    const wchar_t* error = hid_error(device);
    return toUtf8Lossy(error);
}

} // namespace xpstreamdeck
