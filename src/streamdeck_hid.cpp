#include "streamdeck_hid.h"

#include <hidapi.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <vector>

namespace xpstreamdeck {

namespace {

constexpr unsigned short kElgatoVendorId = 0x0fd9;
constexpr std::size_t kInputReportHeaderLength = 4;
constexpr std::size_t kFeatureReportLength = 32;
constexpr std::size_t kImageResetReportLength = 1024;
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

void resetKeyStream(hid_device* device) {
    std::vector<unsigned char> payload(kImageResetReportLength, 0);
    payload[0] = 0x02;
    hid_write(device, payload.data(), payload.size());
}

void setBrightness(hid_device* device, int brightnessPercent) {
    brightnessPercent = std::clamp(brightnessPercent, 0, 100);
    unsigned char payload[kFeatureReportLength] = {};
    payload[0] = 0x03;
    payload[1] = 0x08;
    payload[2] = static_cast<unsigned char>(brightnessPercent);
    hid_send_feature_report(device, payload, sizeof(payload));
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

bool StreamDeckHidBackend::start(const std::string& preferredSerial, int brightnessPercent, std::string* errorMessage) {
    stop();

    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!openDeviceLocked(preferredSerial, brightnessPercent, errorMessage)) {
            return false;
        }
        stopRequested_.store(false);
    }

    readerThread_ = std::thread(&StreamDeckHidBackend::workerLoop, this);
    return true;
}

void StreamDeckHidBackend::stop() {
    stopRequested_.store(true);

    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    std::lock_guard<std::mutex> guard(mutex_);
    closeDeviceLocked();
    if (!currentDeck_.connected) {
        statusLine_ = "Deck backend idle.";
    }
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
            continue;
        }

        const std::string serial = toUtf8Lossy(current->serial_number);
        if (!preferredSerial.empty() && serial != preferredSerial) {
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
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    }

    device_ = hid_open_path(selected.path.c_str());
    if (device_ == nullptr) {
        const std::string message = "Failed to open Stream Deck: " + selected.path;
        setStatusLocked(message);
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    }

    resetKeyStream(device_);
    setBrightness(device_, brightnessPercent);

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
    while (!stopRequested_.load()) {
        hid_device* device = nullptr;
        std::size_t keyCount = 0;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            device = device_;
            keyCount = currentDeck_.key_count;
        }

        if (device == nullptr || keyCount == 0) {
            break;
        }

        std::vector<unsigned char> report(kInputReportHeaderLength + keyCount, 0);
        const int bytesRead = hid_read_timeout(device, report.data(), report.size(), kReadTimeoutMs);
        if (bytesRead < 0) {
            std::lock_guard<std::mutex> guard(mutex_);
            const std::string error = deviceErrorString(device_);
            closeDeviceLocked();
            setStatusLocked(error.empty() ? "Deck disconnected or read failed." : ("Deck read failed: " + error));
            break;
        }

        if (bytesRead == 0 || static_cast<std::size_t>(bytesRead) < (kInputReportHeaderLength + keyCount)) {
            continue;
        }

        StreamDeckKeyEventCallback callback;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            callback = eventCallback_;
        }

        for (std::size_t index = 0; index < keyCount; ++index) {
            const bool pressed = report[kInputReportHeaderLength + index] != 0;
            if (index >= lastKeyStates_.size() || lastKeyStates_[index] == pressed) {
                continue;
            }

            lastKeyStates_[index] = pressed;
            if (callback) {
                callback(static_cast<int>(index), pressed);
            }
        }
    }
}

void StreamDeckHidBackend::setStatusLocked(const std::string& status) {
    statusLine_ = status;
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
