#include "plugin_config.h"

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "plugin_utils.h"
#include "streamdeck_hid.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kNativePathsFeature = "XPLM_USE_NATIVE_PATHS";
constexpr int kWindowWidth = 680;
constexpr int kWindowHeight = 390;
constexpr float kFlightLoopInterval = 0.05f;
constexpr float kDeckReconnectRetrySeconds = 2.0f;
constexpr float kProfileRetrySeconds = 5.0f;
constexpr float kPulseDurationSeconds = 0.08f;

enum class InternalCommand {
    ToggleWindow,
    ReloadPrefs,
    TestFirstBinding,
};

void* commandRefcon(InternalCommand command) {
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(command) + 1);
}

InternalCommand decodeCommandRefcon(void* refcon) {
    return static_cast<InternalCommand>(reinterpret_cast<std::intptr_t>(refcon) - 1);
}

enum class ActionMode {
    Once,
    Hold,
    Pulse,
};

enum class LogLevel {
    Error,
    Warn,
    Info,
    Debug,
};

struct Prefs {
    bool enabled = true;
    bool logfileEnabled = true;
    bool debugLogging = false;
    bool keyImagesEnabled = true;
    bool showWindowOnStart = false;
    bool hidAutoConnect = true;
    std::string activeProfile = "default";
    std::string deckSerial;
    int brightness = 75;
};

struct Paths {
    fs::path systemRoot;
    fs::path pluginDir;
    fs::path prefsFile;
    fs::path profilesDir;
    fs::path fallbackProfileFile;
    fs::path activeProfileFile;
    fs::path logDir;
    fs::path logFile;
};

struct ProfileCandidate {
    fs::path path;
    std::string profileName;
    std::vector<std::string> tailnums;
};

struct ActionBinding {
    int keyIndex = -1;
    std::string commandPath;
    std::string label;
    ActionMode mode = ActionMode::Once;
    XPLMCommandRef command = nullptr;
    bool active = false;
    float pulseEndAt = 0.0f;
};

struct KeyStyle {
    bool hasTextScale = false;
    int textScale = 0;
    bool hasBackground = false;
    xpstreamdeck::RgbColor background{};
    bool hasForeground = false;
    xpstreamdeck::RgbColor foreground{};
    bool hasAccent = false;
    xpstreamdeck::RgbColor accent{};
};

struct PendingKeyEvent {
    int keyIndex = -1;
    bool pressed = false;
};

struct PendingLogEntry {
    LogLevel level = LogLevel::Info;
    std::string component;
    std::string message;
    std::string timestamp;
};

Prefs g_prefs;
Paths g_paths;
std::vector<ActionBinding> g_bindings;
std::map<int, std::string> g_keyLabels;
std::map<int, KeyStyle> g_keyStyles;
std::ofstream g_logFile;
std::mutex g_logMutex;
std::deque<PendingLogEntry> g_pendingLogEntries;
std::thread::id g_mainThreadId;
XPLMWindowID g_window = nullptr;
XPLMMenuID g_menu = nullptr;
int g_pluginsMenuItem = -1;
XPLMCommandRef g_cmdToggleWindow = nullptr;
XPLMCommandRef g_cmdReloadPrefs = nullptr;
XPLMCommandRef g_cmdTestFirstBinding = nullptr;
bool g_windowVisible = false;
bool g_pluginCurrentlyEnabled = false;
bool g_flightLoopRegistered = false;
bool g_deckWasConnected = false;
bool g_deckReconnectPending = false;
bool g_profileSelectionPending = false;
int g_resolvedBindings = 0;
float g_nextDeckReconnectAt = 0.0f;
float g_nextProfileSelectionAt = 0.0f;
std::vector<ProfileCandidate> g_profileCandidates;
std::string g_activeProfileName = "default";
std::string g_activeProfileSource = "fallback";
std::string g_lastKnownTailnum = "<unknown>";
std::string g_statusLine = "Plugin initialized.";
std::string g_lastKeyEventLine = "No key events yet.";
int g_deckReconnectAttempts = 0;
std::mutex g_eventMutex;
std::deque<PendingKeyEvent> g_pendingKeyEvents;
xpstreamdeck::StreamDeckHidBackend g_deckBackend;

std::string pathToDisplay(const fs::path& p) {
    return p.generic_string();
}

std::string sanitizeProfileName(std::string name) {
    name = trimString(name);
    if (name.empty()) {
        return "default";
    }
    for (char& c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
        if (!ok) {
            c = '_';
        }
    }
    return name;
}

std::string unescapeProfileText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool escaping = false;
    for (char ch : text) {
        if (!escaping) {
            if (ch == '\\') {
                escaping = true;
            } else {
                out.push_back(ch);
            }
            continue;
        }

        switch (ch) {
        case 'n':
            out.push_back('\n');
            break;
        case '\\':
            out.push_back('\\');
            break;
        default:
            out.push_back(ch);
            break;
        }
        escaping = false;
    }
    if (escaping) {
        out.push_back('\\');
    }
    return out;
}

std::string rgbColorToHex(const xpstreamdeck::RgbColor& color) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.r, color.g, color.b);
    return std::string(buffer);
}

bool tryParseHexColor(const std::string& value, xpstreamdeck::RgbColor& out) {
    if (value.size() != 6) {
        return false;
    }

    auto hexByte = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    };

    std::array<int, 6> digits{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        digits[i] = hexByte(value[i]);
        if (digits[i] < 0) {
            return false;
        }
    }

    out.r = static_cast<std::uint8_t>((digits[0] << 4) | digits[1]);
    out.g = static_cast<std::uint8_t>((digits[2] << 4) | digits[3]);
    out.b = static_cast<std::uint8_t>((digits[4] << 4) | digits[5]);
    return true;
}

bool tryParseRgbColor(const std::string& raw, xpstreamdeck::RgbColor& out) {
    std::string value = trimString(raw);
    if (value.empty()) {
        return false;
    }

    if (!value.empty() && value.front() == '#') {
        value.erase(value.begin());
    }
    if (tryParseHexColor(value, out)) {
        return true;
    }

    std::string normalized = toLowerCopy(trimString(raw));
    static const std::array<std::pair<const char*, xpstreamdeck::RgbColor>, 12> kNamedColors = {{
        {"white", {245, 247, 250}},
        {"black", {10, 14, 20}},
        {"gray", {62, 70, 82}},
        {"grey", {62, 70, 82}},
        {"slate", {47, 76, 120}},
        {"blue", {36, 93, 168}},
        {"cyan", {36, 128, 156}},
        {"teal", {33, 120, 114}},
        {"green", {36, 118, 76}},
        {"amber", {130, 88, 32}},
        {"orange", {154, 82, 28}},
        {"red", {120, 36, 36}},
    }};
    for (const auto& [name, color] : kNamedColors) {
        if (normalized == name) {
            out = color;
            return true;
        }
    }

    std::string first;
    std::string second;
    std::string third;
    if (splitOnce(value, ',', first, second) && splitOnce(second, ',', second, third)) {
        try {
            const int r = std::stoi(trimString(first));
            const int g = std::stoi(trimString(second));
            const int b = std::stoi(trimString(third));
            if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
                return false;
            }
            out = {
                static_cast<std::uint8_t>(r),
                static_cast<std::uint8_t>(g),
                static_cast<std::uint8_t>(b),
            };
            return true;
        } catch (...) {
            return false;
        }
    }

    return false;
}

std::string defaultLabelFromCommandPath(const std::string& commandPath) {
    if (commandPath.empty()) {
        return {};
    }

    std::string tail = commandPath;
    const auto slash = tail.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < tail.size()) {
        tail = tail.substr(slash + 1);
    }

    for (char& ch : tail) {
        if (ch == '_' || ch == '-') {
            ch = ' ';
        }
    }

    std::string cleaned;
    cleaned.reserve(tail.size());
    bool lastWasSpace = false;
    for (unsigned char raw : tail) {
        char ch = static_cast<char>(raw);
        if (std::isspace(raw)) {
            if (!cleaned.empty() && !lastWasSpace) {
                cleaned.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        lastWasSpace = false;
        cleaned.push_back(static_cast<char>(std::toupper(raw)));
    }

    while (!cleaned.empty() && cleaned.back() == ' ') {
        cleaned.pop_back();
    }

    if (cleaned.size() > 14) {
        auto split = cleaned.find(' ');
        if (split != std::string::npos && split < 8) {
            return cleaned.substr(0, split) + "\n" + trimString(cleaned.substr(split + 1));
        }
    }
    return cleaned;
}

std::string nowString() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm tmNow{};
#if IBM
    localtime_s(&tmNow, &raw);
#else
    localtime_r(&raw, &tmNow);
#endif
    char buf[64] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
    return std::string(buf);
}

const char* logLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Debug:
        return "DEBUG";
    default:
        return "INFO";
    }
}

bool shouldLogLevel(LogLevel level) {
    return level != LogLevel::Debug || g_prefs.debugLogging;
}

bool onMainThread() {
    return g_mainThreadId == std::thread::id{} || std::this_thread::get_id() == g_mainThreadId;
}

std::string formatLogLine(const PendingLogEntry& entry) {
    return "[" + entry.timestamp + "] [" + logLevelName(entry.level) + "] [" + entry.component + "] " + entry.message;
}

void writeFormattedLogLineUnlocked(const std::string& full) {
    XPLMDebugString((std::string(PLUGIN_LOG_PREFIX) + ": " + full + "\n").c_str());
    if (g_logFile.is_open()) {
        g_logFile << full << '\n';
        g_logFile.flush();
    }
}

void flushPendingLogEntries() {
    if (!onMainThread()) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_logMutex);
    while (!g_pendingLogEntries.empty()) {
        writeFormattedLogLineUnlocked(formatLogLine(g_pendingLogEntries.front()));
        g_pendingLogEntries.pop_front();
    }
}

void logMessage(LogLevel level, const std::string& component, const std::string& message) {
    if (!shouldLogLevel(level)) {
        return;
    }

    PendingLogEntry entry;
    entry.level = level;
    entry.component = component;
    entry.message = message;
    entry.timestamp = nowString();

    std::lock_guard<std::mutex> guard(g_logMutex);
    if (!onMainThread()) {
        g_pendingLogEntries.push_back(std::move(entry));
        return;
    }
    writeFormattedLogLineUnlocked(formatLogLine(entry));
}

void logError(const std::string& component, const std::string& message) {
    logMessage(LogLevel::Error, component, message);
}

void logWarn(const std::string& component, const std::string& message) {
    logMessage(LogLevel::Warn, component, message);
}

void logInfo(const std::string& component, const std::string& message) {
    logMessage(LogLevel::Info, component, message);
}

void logDebug(const std::string& component, const std::string& message) {
    logMessage(LogLevel::Debug, component, message);
}

std::string boolOnOff(bool value) {
    return value ? "on" : "off";
}

std::string prefsSummary() {
    std::ostringstream out;
    out << "enabled=" << bool01(g_prefs.enabled)
        << " logfile=" << bool01(g_prefs.logfileEnabled)
        << " debug=" << bool01(g_prefs.debugLogging)
        << " key_images=" << bool01(g_prefs.keyImagesEnabled)
        << " show_window=" << bool01(g_prefs.showWindowOnStart)
        << " auto_connect=" << bool01(g_prefs.hidAutoConnect)
        << " profile=" << g_prefs.activeProfile
        << " serial=" << (g_prefs.deckSerial.empty() ? std::string("<auto>") : g_prefs.deckSerial)
        << " brightness=" << g_prefs.brightness;
    return out.str();
}

std::string bindingSummary(const ActionBinding& binding) {
    std::ostringstream out;
    const char* modeName = "once";
    switch (binding.mode) {
    case ActionMode::Hold:
        modeName = "hold";
        break;
    case ActionMode::Pulse:
        modeName = "pulse";
        break;
    case ActionMode::Once:
    default:
        break;
    }
    out << "key." << binding.keyIndex
        << " command=" << binding.commandPath
        << " mode=" << modeName
        << " label='" << binding.label << "'"
        << " resolved=" << (binding.command != nullptr ? "1" : "0")
        << " active=" << (binding.active ? "1" : "0");
    return out.str();
}

std::string keyStyleSummary(const KeyStyle& style) {
    std::ostringstream out;
    bool wrote = false;
    if (style.hasTextScale) {
        out << "text_scale=" << style.textScale;
        wrote = true;
    }
    if (style.hasBackground) {
        if (wrote) {
            out << ' ';
        }
        out << "bg=" << rgbColorToHex(style.background);
        wrote = true;
    }
    if (style.hasForeground) {
        if (wrote) {
            out << ' ';
        }
        out << "fg=" << rgbColorToHex(style.foreground);
        wrote = true;
    }
    if (style.hasAccent) {
        if (wrote) {
            out << ' ';
        }
        out << "accent=" << rgbColorToHex(style.accent);
        wrote = true;
    }
    return wrote ? out.str() : std::string("<default>");
}

std::size_t pendingLogCount() {
    std::lock_guard<std::mutex> guard(g_logMutex);
    return g_pendingLogEntries.size();
}

fs::path makeNativePath(const char* raw) {
    return fs::u8path(trimNull(std::string(raw ? raw : "")));
}

void refreshBasePaths() {
    char systemPath[1024] = {};
    char prefsPath[1024] = {};
    XPLMGetSystemPath(systemPath);
    XPLMGetPrefsPath(prefsPath);

    g_paths.systemRoot = makeNativePath(systemPath);
    const fs::path prefsFile = makeNativePath(prefsPath);
    const fs::path prefsDir = prefsFile.parent_path();

    g_paths.pluginDir = g_paths.systemRoot / "Resources" / "plugins" / PLUGIN_DIR;
    g_paths.prefsFile = prefsDir / PLUGIN_PREFS_FILE;
    g_paths.profilesDir = g_paths.pluginDir / "profiles";
    g_paths.logDir = g_paths.pluginDir / "log";
    g_paths.logFile = g_paths.logDir / PLUGIN_LOG_NAME;

    logDebug("paths",
        "system_root=" + pathToDisplay(g_paths.systemRoot) +
        " plugin_dir=" + pathToDisplay(g_paths.pluginDir) +
        " prefs=" + pathToDisplay(g_paths.prefsFile) +
        " profiles=" + pathToDisplay(g_paths.profilesDir) +
        " log=" + pathToDisplay(g_paths.logFile));
}

void refreshActiveProfilePath() {
    g_prefs.activeProfile = sanitizeProfileName(g_prefs.activeProfile);
    g_paths.fallbackProfileFile = g_paths.profilesDir / (g_prefs.activeProfile + ".cfg");
    g_paths.activeProfileFile = g_paths.fallbackProfileFile;
    g_activeProfileName = g_prefs.activeProfile;
    g_activeProfileSource = "fallback";

    logDebug("profile",
        "fallback_profile=" + g_activeProfileName +
        " path=" + pathToDisplay(g_paths.fallbackProfileFile));
}

void ensureDir(const fs::path& dir) {
    std::error_code ec;
    const bool created = fs::create_directories(dir, ec);
    if (ec) {
        logError("fs", "Failed to create directory: " + pathToDisplay(dir) + " (" + ec.message() + ")");
        return;
    }
    if (created) {
        logInfo("fs", "Created directory: " + pathToDisplay(dir));
    }
}

void writeDefaultProfileIfMissing() {
    if (fs::exists(g_paths.fallbackProfileFile)) {
        logDebug("profile", "Default profile already exists: " + pathToDisplay(g_paths.fallbackProfileFile));
        return;
    }
    ensureDir(g_paths.profilesDir);
    std::ofstream out(g_paths.fallbackProfileFile);
    if (!out.is_open()) {
        logError("profile", "Failed to create default profile: " + pathToDisplay(g_paths.fallbackProfileFile));
        return;
    }
    out
        << "profile_id=default\n"
        << "# key.<index>=<command>|<mode>\n"
        << "# label.<index>=TEXT or TEXT\\nTEXT\n"
        << "# text_scale.<index>=1..6 (caps auto text size)\n"
        << "# bg.<index>=#RRGGBB or named-color\n"
        << "# fg.<index>=#RRGGBB or named-color\n"
        << "# accent.<index>=#RRGGBB or named-color\n"
        << "# mode: once | hold | pulse\n"
        << '\n'
        << "label.0=PAUSE\n"
        << "bg.0=#4A4F57\n"
        << "key.0=sim/operation/pause_toggle|once\n"
        << "label.1=FLAPS\\nDOWN\n"
        << "bg.1=#815820\n"
        << "key.1=sim/flight_controls/flaps_down|once\n"
        << "label.2=FLAPS\\nUP\n"
        << "bg.2=#815820\n"
        << "key.2=sim/flight_controls/flaps_up|once\n"
        << "label.3=BRAKES\n"
        << "bg.3=#7A2F2F\n"
        << "key.3=sim/flight_controls/brakes_toggle_max|once\n"
        << "label.4=REV\\nHOLD\n"
        << "bg.4=#815820\n"
        << "key.4=sim/engines/thrust_reverse_hold|hold\n";
    logInfo("profile", "Created default profile: " + pathToDisplay(g_paths.fallbackProfileFile));
}

std::string readTailnum() {
    static XPLMDataRef tailRef = XPLMFindDataRef("sim/aircraft/view/acf_tailnum");
    static bool loggedMissingTailRef = false;
    if (!tailRef) {
        if (!loggedMissingTailRef) {
            loggedMissingTailRef = true;
            logWarn("profile", "DataRef sim/aircraft/view/acf_tailnum not found.");
        }
        return {};
    }

    char buf[256]{};
    const int n = XPLMGetDatab(tailRef, buf, 0, sizeof(buf) - 1);
    if (n <= 0) {
        return {};
    }

    std::string raw(buf, static_cast<std::size_t>(n));
    const auto nul = raw.find('\0');
    if (nul != std::string::npos) {
        raw.resize(nul);
    }
    return trimString(raw);
}

void clearProfileSelectionRetry() {
    g_profileSelectionPending = false;
    g_nextProfileSelectionAt = 0.0f;
}

void scheduleProfileSelectionRetry(float now, const std::string& reason) {
    g_profileSelectionPending = true;
    g_nextProfileSelectionAt = now + kProfileRetrySeconds;
    if (!reason.empty()) {
        logWarn("profile", reason + " Next attempt in " + std::to_string(static_cast<int>(kProfileRetrySeconds)) + "s.");
    }
}

void loadProfileCandidates() {
    g_profileCandidates.clear();

    if (!fs::exists(g_paths.profilesDir)) {
        logWarn("profile", "Profiles directory does not exist: " + pathToDisplay(g_paths.profilesDir));
        return;
    }

    logInfo("profile", "Scanning profile candidates in " + pathToDisplay(g_paths.profilesDir));

    std::vector<fs::path> profilePaths;
    for (const auto& entry : fs::directory_iterator(g_paths.profilesDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".cfg") {
            continue;
        }
        profilePaths.push_back(entry.path());
    }

    std::sort(profilePaths.begin(), profilePaths.end());

    std::map<std::string, std::string> tailnumOwner;
    for (const auto& path : profilePaths) {
        ProfileCandidate candidate;
        candidate.path = path;
        candidate.profileName = path.stem().string();

        std::ifstream in(path);
        if (!in.is_open()) {
            logError("profile", "Failed to inspect profile metadata: " + pathToDisplay(path));
            continue;
        }

        std::string line;
        int lineNumber = 0;
        while (std::getline(in, line)) {
            ++lineNumber;
            line = trimString(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }

            std::string lhs;
            std::string rhs;
            if (!splitOnce(line, '=', lhs, rhs)) {
                logWarn("profile", "Ignoring malformed metadata line " + std::to_string(lineNumber) + " in " + pathToDisplay(path) + ": " + line);
                continue;
            }

            const std::string key = toLowerCopy(trimString(lhs));
            const std::string value = trimString(rhs);
            if (key == "profile_id" && !value.empty()) {
                candidate.profileName = value;
            } else if (key == "tailnum" && !value.empty()) {
                candidate.tailnums.push_back(value);
            } else if (key == "profile_id" || key == "tailnum") {
                logWarn("profile", "Ignoring empty metadata value for '" + key + "' in " + pathToDisplay(path) + ":" + std::to_string(lineNumber));
            }
        }

        for (const auto& tailnum : candidate.tailnums) {
            const auto existing = tailnumOwner.find(tailnum);
            if (existing != tailnumOwner.end()) {
                logWarn("profile", "Duplicate tailnum '" + tailnum + "' in " + pathToDisplay(path) + " (already in " + existing->second + ")");
            } else {
                tailnumOwner.emplace(tailnum, pathToDisplay(path));
            }
        }

        std::ostringstream summary;
        summary << "Candidate profile id=" << candidate.profileName
                << " file=" << pathToDisplay(path)
                << " tailnums=" << candidate.tailnums.size();
        logDebug("profile", summary.str());
        g_profileCandidates.push_back(std::move(candidate));
    }

    logInfo("profile", "Loaded " + std::to_string(g_profileCandidates.size()) + " profile candidate(s).");
}

const ProfileCandidate* findProfileCandidateByTailnum(const std::string& tailnum) {
    for (const auto& candidate : g_profileCandidates) {
        for (const auto& candidateTailnum : candidate.tailnums) {
            if (candidateTailnum == tailnum) {
                return &candidate;
            }
        }
    }
    return nullptr;
}

void savePrefs() {
    std::ofstream out(g_paths.prefsFile);
    if (!out.is_open()) {
        logError("prefs", "Failed to write prefs: " + pathToDisplay(g_paths.prefsFile));
        return;
    }
    out
        << "enabled=" << bool01(g_prefs.enabled) << '\n'
        << "logfile_enabled=" << bool01(g_prefs.logfileEnabled) << '\n'
        << "debug_logging=" << bool01(g_prefs.debugLogging) << '\n'
        << "key_images_enabled=" << bool01(g_prefs.keyImagesEnabled) << '\n'
        << "show_window_on_start=" << bool01(g_prefs.showWindowOnStart) << '\n'
        << "hid_auto_connect=" << bool01(g_prefs.hidAutoConnect) << '\n'
        << "active_profile=" << g_prefs.activeProfile << '\n'
        << "deck_serial=" << g_prefs.deckSerial << '\n'
        << "brightness=" << g_prefs.brightness << '\n';
    logInfo("prefs", "Saved prefs to " + pathToDisplay(g_paths.prefsFile));
    logDebug("prefs", prefsSummary());
}

void loadPrefs() {
    g_prefs = Prefs{};
    if (!fs::exists(g_paths.prefsFile)) {
        logWarn("prefs", "Prefs file not found, writing defaults: " + pathToDisplay(g_paths.prefsFile));
        savePrefs();
        return;
    }

    std::ifstream in(g_paths.prefsFile);
    if (!in.is_open()) {
        logError("prefs", "Failed to open prefs: " + pathToDisplay(g_paths.prefsFile));
        return;
    }

    logInfo("prefs", "Loading prefs from " + pathToDisplay(g_paths.prefsFile));
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        line = trimString(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::string key;
        std::string value;
        if (!splitOnce(line, '=', key, value)) {
            logWarn("prefs", "Ignoring malformed prefs line " + std::to_string(lineNumber) + ": " + line);
            continue;
        }

        key = toLowerCopy(trimString(key));
        value = trimString(value);

        if (key == "enabled") {
            if (!parseBool(value, g_prefs.enabled)) {
                logWarn("prefs", "Invalid boolean for enabled on line " + std::to_string(lineNumber) + ": " + value);
            }
        } else if (key == "logfile_enabled") {
            if (!parseBool(value, g_prefs.logfileEnabled)) {
                logWarn("prefs", "Invalid boolean for logfile_enabled on line " + std::to_string(lineNumber) + ": " + value);
            }
        } else if (key == "debug_logging") {
            if (!parseBool(value, g_prefs.debugLogging)) {
                logWarn("prefs", "Invalid boolean for debug_logging on line " + std::to_string(lineNumber) + ": " + value);
            }
        } else if (key == "key_images_enabled") {
            if (!parseBool(value, g_prefs.keyImagesEnabled)) {
                logWarn("prefs", "Invalid boolean for key_images_enabled on line " + std::to_string(lineNumber) + ": " + value);
            }
        } else if (key == "show_window_on_start") {
            if (!parseBool(value, g_prefs.showWindowOnStart)) {
                logWarn("prefs", "Invalid boolean for show_window_on_start on line " + std::to_string(lineNumber) + ": " + value);
            }
        } else if (key == "hid_auto_connect") {
            if (!parseBool(value, g_prefs.hidAutoConnect)) {
                logWarn("prefs", "Invalid boolean for hid_auto_connect on line " + std::to_string(lineNumber) + ": " + value);
            }
        } else if (key == "active_profile") {
            g_prefs.activeProfile = value;
        } else if (key == "deck_serial") {
            g_prefs.deckSerial = value;
        } else if (key == "brightness") {
            try {
                g_prefs.brightness = std::stoi(value);
            } catch (...) {
                logWarn("prefs", "Invalid integer for brightness on line " + std::to_string(lineNumber) + ": " + value);
            }
        } else {
            logWarn("prefs", "Ignoring unknown prefs key '" + key + "' on line " + std::to_string(lineNumber));
        }
    }

    g_prefs.activeProfile = sanitizeProfileName(g_prefs.activeProfile);
    g_prefs.brightness = std::clamp(g_prefs.brightness, 0, 100);
    logInfo("prefs", "Prefs loaded.");
    logDebug("prefs", prefsSummary());
}

void reopenLogFile() {
    flushPendingLogEntries();
    {
        std::lock_guard<std::mutex> guard(g_logMutex);
        if (g_logFile.is_open()) {
            g_logFile.close();
        }
    }
    if (!g_prefs.logfileEnabled) {
        logInfo("log", "File logging disabled.");
        return;
    }
    ensureDir(g_paths.logDir);
    {
        std::lock_guard<std::mutex> guard(g_logMutex);
        g_logFile.open(g_paths.logFile, std::ios::app);
    }
    if (!g_logFile.is_open()) {
        XPLMDebugString((std::string(PLUGIN_LOG_PREFIX) + ": failed to open log file\n").c_str());
        return;
    }
    logInfo("log", "Opened log file: " + pathToDisplay(g_paths.logFile));
}

bool tryParseActionMode(const std::string& raw, ActionMode& out) {
    const std::string normalized = toLowerCopy(trimString(raw));
    if (normalized.empty() || normalized == "once") {
        out = ActionMode::Once;
        return true;
    }
    if (normalized == "hold") {
        out = ActionMode::Hold;
        return true;
    }
    if (normalized == "pulse") {
        out = ActionMode::Pulse;
        return true;
    }
    return false;
}

const char* actionModeName(ActionMode mode) {
    switch (mode) {
    case ActionMode::Hold:
        return "hold";
    case ActionMode::Pulse:
        return "pulse";
    case ActionMode::Once:
    default:
        return "once";
    }
}

LogLevel mapBackendLogLevel(xpstreamdeck::StreamDeckBackendLogLevel level) {
    switch (level) {
    case xpstreamdeck::StreamDeckBackendLogLevel::Error:
        return LogLevel::Error;
    case xpstreamdeck::StreamDeckBackendLogLevel::Warn:
        return LogLevel::Warn;
    case xpstreamdeck::StreamDeckBackendLogLevel::Info:
        return LogLevel::Info;
    case xpstreamdeck::StreamDeckBackendLogLevel::Debug:
    default:
        return LogLevel::Debug;
    }
}

void handleDeckBackendLog(xpstreamdeck::StreamDeckBackendLogLevel level, const std::string& message) {
    logMessage(mapBackendLogLevel(level), "deck-hid", message);
}

void releaseActiveBindings() {
    for (auto& binding : g_bindings) {
        if (binding.active && binding.command != nullptr) {
            XPLMCommandEnd(binding.command);
            logDebug("dispatch", "Released active binding during cleanup: " + bindingSummary(binding));
            binding.active = false;
        }
        binding.pulseEndAt = 0.0f;
    }
}

ActionBinding* findBindingForKey(int keyIndex) {
    for (auto& binding : g_bindings) {
        if (binding.keyIndex == keyIndex) {
            return &binding;
        }
    }
    return nullptr;
}

void resolveBindings() {
    g_resolvedBindings = 0;
    for (auto& binding : g_bindings) {
        binding.command = XPLMFindCommand(binding.commandPath.c_str());
        binding.active = false;
        binding.pulseEndAt = 0.0f;
        if (binding.label.empty()) {
            binding.label = defaultLabelFromCommandPath(binding.commandPath);
        }
        if (binding.command != nullptr) {
            ++g_resolvedBindings;
            logDebug("profile", "Resolved binding: " + bindingSummary(binding));
        } else {
            logWarn("profile", "Unresolved command in profile: " + bindingSummary(binding));
        }
    }

    std::ostringstream summary;
    summary << "Loaded " << g_bindings.size() << " binding(s), resolved " << g_resolvedBindings << '.';
    g_statusLine = summary.str();
    logInfo("profile", g_statusLine);
}

void loadProfile() {
    releaseActiveBindings();
    g_bindings.clear();
    g_keyLabels.clear();
    g_keyStyles.clear();

    logInfo("profile", "Loading profile from " + pathToDisplay(g_paths.activeProfileFile));

    std::ifstream in(g_paths.activeProfileFile);
    if (!in.is_open()) {
        g_statusLine = "Failed to open active profile.";
        logError("profile", "Failed to open profile: " + pathToDisplay(g_paths.activeProfileFile));
        return;
    }

    std::string line;
    int lineNumber = 0;
    std::map<int, int> bindingLineByKey;
    std::map<int, int> labelLineByKey;
    std::map<int, int> textScaleLineByKey;
    std::map<int, int> bgLineByKey;
    std::map<int, int> fgLineByKey;
    std::map<int, int> accentLineByKey;
    while (std::getline(in, line)) {
        ++lineNumber;
        line = trimString(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::string lhs;
        std::string rhs;
        if (!splitOnce(line, '=', lhs, rhs)) {
            logWarn("profile", "Ignoring malformed profile line " + std::to_string(lineNumber) + ": " + line);
            continue;
        }

        lhs = toLowerCopy(trimString(lhs));
        rhs = trimString(rhs);

        if (lhs.rfind("key.", 0) != 0) {
            if (lhs == "profile_id" || lhs == "tailnum") {
                logDebug("profile", "Ignoring metadata directive '" + lhs + "' on line " + std::to_string(lineNumber));
                continue;
            }
            if (lhs.rfind("label.", 0) == 0) {
                const std::string indexString = lhs.substr(6);
                int keyIndex = -1;
                try {
                    keyIndex = std::stoi(indexString);
                } catch (...) {
                    logWarn("profile", "Invalid label index in profile on line " + std::to_string(lineNumber) + ": " + indexString);
                    continue;
                }

                if (const auto existing = labelLineByKey.find(keyIndex); existing != labelLineByKey.end()) {
                    logWarn("profile", "Duplicate label for key." + std::to_string(keyIndex) + " on line " + std::to_string(lineNumber) +
                        " (previous line " + std::to_string(existing->second) + ")");
                }
                g_keyLabels[keyIndex] = unescapeProfileText(rhs);
                labelLineByKey[keyIndex] = lineNumber;
                logDebug("profile", "Parsed label key." + std::to_string(keyIndex) + "='" + g_keyLabels[keyIndex] + "'");
                continue;
            }

            if (lhs.rfind("text_scale.", 0) == 0) {
                const std::string indexString = lhs.substr(11);
                int keyIndex = -1;
                try {
                    keyIndex = std::stoi(indexString);
                } catch (...) {
                    logWarn("profile", "Invalid text_scale index in profile on line " + std::to_string(lineNumber) + ": " + indexString);
                    continue;
                }

                int textScale = 0;
                try {
                    textScale = std::stoi(rhs);
                } catch (...) {
                    logWarn("profile", "Invalid text_scale value '" + rhs + "' for key." + std::to_string(keyIndex) +
                        " on line " + std::to_string(lineNumber));
                    continue;
                }
                if (textScale < 1 || textScale > 6) {
                    logWarn("profile", "text_scale for key." + std::to_string(keyIndex) +
                        " on line " + std::to_string(lineNumber) + " must be between 1 and 6");
                    continue;
                }

                if (const auto existing = textScaleLineByKey.find(keyIndex); existing != textScaleLineByKey.end()) {
                    logWarn("profile", "Duplicate text_scale for key." + std::to_string(keyIndex) + " on line " + std::to_string(lineNumber) +
                        " (previous line " + std::to_string(existing->second) + ")");
                }

                auto& style = g_keyStyles[keyIndex];
                style.hasTextScale = true;
                style.textScale = textScale;
                textScaleLineByKey[keyIndex] = lineNumber;
                logDebug("profile", "Parsed text_scale key." + std::to_string(keyIndex) + "=" + std::to_string(textScale));
                continue;
            }

            auto parseStyleDirective = [&](const char* directiveName,
                                           const std::string& prefix,
                                           std::map<int, int>& lineMap,
                                           bool KeyStyle::* flagMember,
                                           xpstreamdeck::RgbColor KeyStyle::* colorMember) -> bool {
                if (lhs.rfind(prefix, 0) != 0) {
                    return false;
                }

                const std::string indexString = lhs.substr(prefix.size());
                int keyIndex = -1;
                try {
                    keyIndex = std::stoi(indexString);
                } catch (...) {
                    logWarn("profile", "Invalid " + std::string(directiveName) + " index in profile on line " + std::to_string(lineNumber) + ": " + indexString);
                    return true;
                }

                xpstreamdeck::RgbColor color;
                if (!tryParseRgbColor(rhs, color)) {
                    logWarn("profile", "Invalid color '" + rhs + "' for " + std::string(directiveName) + " key." + std::to_string(keyIndex) +
                        " on line " + std::to_string(lineNumber));
                    return true;
                }

                if (const auto existing = lineMap.find(keyIndex); existing != lineMap.end()) {
                    logWarn("profile", "Duplicate " + std::string(directiveName) + " for key." + std::to_string(keyIndex) +
                        " on line " + std::to_string(lineNumber) + " (previous line " + std::to_string(existing->second) + ")");
                }

                auto& style = g_keyStyles[keyIndex];
                style.*flagMember = true;
                style.*colorMember = color;
                lineMap[keyIndex] = lineNumber;
                logDebug("profile", "Parsed " + std::string(directiveName) + " key." + std::to_string(keyIndex) + "=" + rgbColorToHex(color));
                return true;
            };

            if (parseStyleDirective("bg", "bg.", bgLineByKey, &KeyStyle::hasBackground, &KeyStyle::background) ||
                parseStyleDirective("fg", "fg.", fgLineByKey, &KeyStyle::hasForeground, &KeyStyle::foreground) ||
                parseStyleDirective("accent", "accent.", accentLineByKey, &KeyStyle::hasAccent, &KeyStyle::accent)) {
                continue;
            }

            {
                logWarn("profile", "Ignoring unknown profile directive '" + lhs + "' on line " + std::to_string(lineNumber));
                continue;
            }
        }

        const std::string indexString = lhs.substr(4);
        int keyIndex = -1;
        try {
            keyIndex = std::stoi(indexString);
        } catch (...) {
            logWarn("profile", "Invalid key index in profile on line " + std::to_string(lineNumber) + ": " + indexString);
            continue;
        }

        std::string commandPath;
        std::string modeRaw;
        if (!splitOnce(rhs, '|', commandPath, modeRaw)) {
            commandPath = rhs;
            modeRaw = "once";
        }

        commandPath = trimString(commandPath);
        if (commandPath.empty()) {
            logWarn("profile", "Ignoring empty command path on line " + std::to_string(lineNumber));
            continue;
        }

        ActionBinding binding;
        binding.keyIndex = keyIndex;
        binding.commandPath = commandPath;
        if (!tryParseActionMode(modeRaw, binding.mode)) {
            logWarn("profile", "Unknown action mode '" + trimString(modeRaw) + "' for key." + std::to_string(keyIndex) +
                " on line " + std::to_string(lineNumber) + ", defaulting to once");
            binding.mode = ActionMode::Once;
        }
        if (const auto labelIt = g_keyLabels.find(keyIndex); labelIt != g_keyLabels.end()) {
            binding.label = labelIt->second;
        }
        if (const auto existing = bindingLineByKey.find(keyIndex); existing != bindingLineByKey.end()) {
            logWarn("profile", "Duplicate binding for key." + std::to_string(keyIndex) + " on line " + std::to_string(lineNumber) +
                " (previous line " + std::to_string(existing->second) + ")");
        }
        bindingLineByKey[keyIndex] = lineNumber;
        logDebug("profile", "Parsed binding: " + bindingSummary(binding));
        g_bindings.push_back(binding);
    }

    for (auto& binding : g_bindings) {
        if (const auto labelIt = g_keyLabels.find(binding.keyIndex); labelIt != g_keyLabels.end()) {
            binding.label = labelIt->second;
        }
    }

    for (const auto& [keyIndex, style] : g_keyStyles) {
        logDebug("profile", "Parsed style key." + std::to_string(keyIndex) + " " + keyStyleSummary(style));
    }

    resolveBindings();
    logInfo("profile", "Active profile: " + g_activeProfileName + " [" + g_activeProfileSource + "] (" + pathToDisplay(g_paths.activeProfileFile) + ")");
}

std::vector<xpstreamdeck::StreamDeckKeyVisual> buildDeckKeyVisuals() {
    std::map<int, xpstreamdeck::StreamDeckKeyVisual> visuals;

    auto applyStyle = [](xpstreamdeck::StreamDeckKeyVisual& visual, const KeyStyle& style) {
        visual.max_text_scale = style.hasTextScale ? style.textScale : 0;
        visual.has_background = style.hasBackground;
        visual.background = style.background;
        visual.has_foreground = style.hasForeground;
        visual.foreground = style.foreground;
        visual.has_accent = style.hasAccent;
        visual.accent = style.accent;
    };

    for (const auto& [keyIndex, label] : g_keyLabels) {
        auto& visual = visuals[keyIndex];
        visual.key_index = keyIndex;
        visual.label = label;
        visual.has_binding = false;
        visual.resolved = false;
        visual.hold_mode = false;
    }

    for (const auto& [keyIndex, style] : g_keyStyles) {
        auto& visual = visuals[keyIndex];
        visual.key_index = keyIndex;
        applyStyle(visual, style);
    }

    for (const auto& binding : g_bindings) {
        auto& visual = visuals[binding.keyIndex];
        visual.key_index = binding.keyIndex;
        visual.label = binding.label.empty() ? defaultLabelFromCommandPath(binding.commandPath) : binding.label;
        visual.has_binding = true;
        visual.resolved = binding.command != nullptr;
        visual.hold_mode = binding.mode == ActionMode::Hold;
        if (const auto styleIt = g_keyStyles.find(binding.keyIndex); styleIt != g_keyStyles.end()) {
            applyStyle(visual, styleIt->second);
        }
    }

    std::vector<xpstreamdeck::StreamDeckKeyVisual> ordered;
    ordered.reserve(visuals.size());
    for (const auto& [keyIndex, visual] : visuals) {
        (void)keyIndex;
        ordered.push_back(visual);
    }
    logDebug("deck", "Built " + std::to_string(ordered.size()) + " key visual(s) for upload.");
    return ordered;
}

void refreshDeckKeyImages() {
    if (!g_prefs.keyImagesEnabled) {
        logInfo("deck", "Key image upload disabled in prefs.");
        return;
    }

    const auto deck = g_deckBackend.currentDeck();
    if (!deck.connected) {
        logDebug("deck", "Skipping key image refresh because no deck is connected.");
        return;
    }

    logDebug("deck", "Refreshing key images on " + deck.product_name + (deck.serial.empty() ? std::string() : (" [" + deck.serial + "]")));
    std::string errorMessage;
    if (!g_deckBackend.applyKeyVisuals(buildDeckKeyVisuals(), &errorMessage) && !errorMessage.empty()) {
        logError("deck", errorMessage);
        return;
    }
    logDebug("deck", "Key image refresh complete.");
}

void activateProfile(const fs::path& path, const std::string& profileName, const std::string& source) {
    logInfo("profile",
        "Activating profile id=" + profileName +
        " source=" + source +
        " path=" + pathToDisplay(path));
    g_paths.activeProfileFile = path;
    g_activeProfileName = profileName;
    g_activeProfileSource = source;
    loadProfile();
    refreshDeckKeyImages();
}

void selectProfileForAircraft(bool logNoMatch) {
    const std::string tailnum = readTailnum();
    g_lastKnownTailnum = tailnum.empty() ? "<unknown>" : tailnum;
    logInfo("profile", "Selecting profile for tailnum=" + g_lastKnownTailnum);

    if (tailnum.empty()) {
        const bool alreadyPending = g_profileSelectionPending;
        if (g_paths.activeProfileFile != g_paths.fallbackProfileFile) {
            activateProfile(g_paths.fallbackProfileFile, g_prefs.activeProfile, "fallback");
        } else {
            refreshDeckKeyImages();
        }
        scheduleProfileSelectionRetry(XPLMGetElapsedTime(), alreadyPending ? std::string() : "tailnum not available, profile selection deferred.");
        return;
    }

    clearProfileSelectionRetry();

    if (const auto* match = findProfileCandidateByTailnum(tailnum); match != nullptr) {
        logInfo("profile", "Tailnum matched profile id=" + match->profileName + " file=" + pathToDisplay(match->path));
        activateProfile(match->path, match->profileName, "tailnum:" + tailnum);
        return;
    }

    if (logNoMatch) {
        logWarn("profile", "No profile matched tailnum '" + tailnum + "', using fallback profile.");
    } else {
        logInfo("profile", "No profile matched tailnum '" + tailnum + "', using fallback profile.");
    }
    activateProfile(g_paths.fallbackProfileFile, g_prefs.activeProfile, "fallback");
}

bool deckConnectionWanted() {
    return g_pluginCurrentlyEnabled && g_prefs.enabled && (g_prefs.hidAutoConnect || !g_prefs.deckSerial.empty());
}

void clearDeckReconnectState() {
    g_deckReconnectPending = false;
    g_nextDeckReconnectAt = 0.0f;
    g_deckReconnectAttempts = 0;
}

void scheduleDeckReconnect(float now, const std::string& reason) {
    if (!deckConnectionWanted()) {
        clearDeckReconnectState();
        return;
    }

    g_deckReconnectPending = true;
    g_nextDeckReconnectAt = now + kDeckReconnectRetrySeconds;
    g_statusLine = reason.empty() ? "Waiting for Stream Deck connection." : (reason + " Retrying automatically.");
    logWarn("deck",
        "Reconnect scheduled in " + std::to_string(static_cast<int>(kDeckReconnectRetrySeconds)) +
        "s. reason=" + (reason.empty() ? std::string("<none>") : reason));
}

void clearPendingKeyEvents() {
    std::lock_guard<std::mutex> guard(g_eventMutex);
    if (!g_pendingKeyEvents.empty()) {
        logDebug("dispatch", "Clearing " + std::to_string(g_pendingKeyEvents.size()) + " pending key event(s).");
    }
    g_pendingKeyEvents.clear();
}

void queueKeyEvent(int keyIndex, bool pressed) {
    std::lock_guard<std::mutex> guard(g_eventMutex);
    g_pendingKeyEvents.push_back(PendingKeyEvent{keyIndex, pressed});
}

void stopDeckBackend() {
    logInfo("deck", "Stopping deck backend.");
    clearPendingKeyEvents();
    g_deckBackend.stop();
    g_deckWasConnected = false;
    clearDeckReconnectState();
}

bool tryConnectDeckBackend(bool logFailures) {
    g_deckBackend.setEventCallback(queueKeyEvent);
    g_deckBackend.setLogCallback(handleDeckBackendLog);
    ++g_deckReconnectAttempts;
    logInfo("deck",
        std::string(logFailures ? "Connecting" : "Reconnecting") +
        " deck attempt #" + std::to_string(g_deckReconnectAttempts) +
        " serial=" + (g_prefs.deckSerial.empty() ? std::string("<auto>") : g_prefs.deckSerial) +
        " brightness=" + std::to_string(g_prefs.brightness));

    std::string errorMessage;
    if (!g_deckBackend.start(g_prefs.deckSerial, g_prefs.brightness, &errorMessage)) {
        if (!errorMessage.empty()) {
            logWarn("deck",
                std::string(logFailures ? "Connect failed: " : "Reconnect failed: ") + errorMessage);
        }
        scheduleDeckReconnect(XPLMGetElapsedTime(), errorMessage);
        g_deckWasConnected = false;
        return false;
    }

    const auto deck = g_deckBackend.currentDeck();
    std::ostringstream msg;
    msg << "Deck ready: " << deck.product_name;
    if (!deck.serial.empty()) {
        msg << " [" << deck.serial << "]";
    }
    g_statusLine = msg.str();
    logInfo("deck", g_statusLine);
    g_deckWasConnected = true;
    clearDeckReconnectState();
    refreshDeckKeyImages();
    return true;
}

void startDeckBackend() {
    stopDeckBackend();

    if (!g_pluginCurrentlyEnabled) {
        logDebug("deck", "Start skipped because plugin is not enabled.");
        return;
    }

    if (!g_prefs.enabled) {
        g_statusLine = "Deck backend disabled in prefs.";
        logInfo("deck", g_statusLine);
        return;
    }

    if (!g_prefs.hidAutoConnect && g_prefs.deckSerial.empty()) {
        g_statusLine = "Deck auto-connect disabled and no serial configured.";
        logWarn("deck", g_statusLine);
        return;
    }

    tryConnectDeckBackend(true);
}

void reconfigureDeckBackend() {
    logInfo("deck", "Reconfiguring deck backend.");
    stopDeckBackend();
    if (g_pluginCurrentlyEnabled) {
        startDeckBackend();
    }
}

void toggleWindow() {
    g_windowVisible = !g_windowVisible;
    logInfo("ui", std::string("Status window ") + (g_windowVisible ? "shown" : "hidden"));
    if (g_window != nullptr) {
        XPLMSetWindowIsVisible(g_window, g_windowVisible ? 1 : 0);
    }
}

void applyWindowVisibility() {
    if (g_window != nullptr) {
        XPLMSetWindowIsVisible(g_window, g_windowVisible ? 1 : 0);
    }
}

void reloadRuntimeConfig() {
    logInfo("prefs", "Reloading runtime configuration.");
    refreshBasePaths();
    loadPrefs();
    refreshActiveProfilePath();
    ensureDir(g_paths.pluginDir);
    ensureDir(g_paths.profilesDir);
    ensureDir(g_paths.logDir);
    writeDefaultProfileIfMissing();
    loadProfileCandidates();
    reopenLogFile();
    selectProfileForAircraft(false);
    applyWindowVisibility();
    reconfigureDeckBackend();
    logInfo("prefs", "Prefs reloaded from " + pathToDisplay(g_paths.prefsFile));
    logDebug("prefs", prefsSummary());
}

void dispatchBinding(ActionBinding& binding, bool pressed) {
    if (binding.command == nullptr) {
        logWarn("dispatch", "Skipping unresolved binding: " + bindingSummary(binding));
        return;
    }

    switch (binding.mode) {
    case ActionMode::Once:
        if (pressed) {
            XPLMCommandOnce(binding.command);
            logDebug("dispatch", "Executed once command: " + bindingSummary(binding));
        }
        break;
    case ActionMode::Hold:
        if (pressed) {
            if (!binding.active) {
                XPLMCommandBegin(binding.command);
                binding.active = true;
                binding.pulseEndAt = 0.0f;
                logDebug("dispatch", "Began hold command: " + bindingSummary(binding));
            }
        } else if (binding.active) {
            XPLMCommandEnd(binding.command);
            binding.active = false;
            binding.pulseEndAt = 0.0f;
            logDebug("dispatch", "Ended hold command: " + bindingSummary(binding));
        }
        break;
    case ActionMode::Pulse:
        if (pressed) {
            const float now = XPLMGetElapsedTime();
            if (!binding.active) {
                XPLMCommandBegin(binding.command);
                binding.active = true;
                logDebug("dispatch", "Began pulse command: " + bindingSummary(binding));
            } else {
                logDebug("dispatch", "Extended pulse command: " + bindingSummary(binding));
            }
            binding.pulseEndAt = now + kPulseDurationSeconds;
        }
        break;
    }
}

void maintainPulseBindings() {
    const float now = XPLMGetElapsedTime();
    for (auto& binding : g_bindings) {
        if (binding.mode != ActionMode::Pulse || !binding.active || binding.command == nullptr) {
            continue;
        }
        if (binding.pulseEndAt <= 0.0f || now < binding.pulseEndAt) {
            continue;
        }

        XPLMCommandEnd(binding.command);
        binding.active = false;
        binding.pulseEndAt = 0.0f;
        logDebug("dispatch", "Ended pulse command: " + bindingSummary(binding));
    }
}

void processPendingKeyEvents() {
    std::deque<PendingKeyEvent> events;
    {
        std::lock_guard<std::mutex> guard(g_eventMutex);
        events.swap(g_pendingKeyEvents);
    }

    if (!events.empty()) {
        logDebug("dispatch", "Processing " + std::to_string(events.size()) + " pending key event(s).");
    }

    for (const auto& event : events) {
        std::ostringstream msg;
        ActionBinding* binding = findBindingForKey(event.keyIndex);
        if (binding == nullptr) {
            msg << "Deck key." << event.keyIndex << (event.pressed ? " pressed" : " released") << " -> unbound";
            g_lastKeyEventLine = msg.str();
            logWarn("dispatch", g_lastKeyEventLine);
            continue;
        }

        dispatchBinding(*binding, event.pressed);
        msg << "Deck key." << event.keyIndex << (event.pressed ? " pressed" : " released")
            << " -> " << binding->commandPath << " (" << actionModeName(binding->mode) << ")";
        g_lastKeyEventLine = msg.str();
        logDebug("dispatch", g_lastKeyEventLine);
    }
}

void maintainDeckBackend() {
    const auto deck = g_deckBackend.currentDeck();
    const float now = XPLMGetElapsedTime();

    if (deck.connected) {
        g_deckWasConnected = true;
        clearDeckReconnectState();
        return;
    }

    if (g_deckWasConnected) {
        g_deckWasConnected = false;
        clearPendingKeyEvents();
        releaseActiveBindings();
        g_lastKeyEventLine = "Deck disconnected.";
        logWarn("deck", "Stream Deck disconnected. backend_status=" + g_deckBackend.statusLine());
        scheduleDeckReconnect(now, "Stream Deck disconnected.");
        return;
    }

    if (!deckConnectionWanted()) {
        clearDeckReconnectState();
        return;
    }

    if (!g_deckReconnectPending) {
        scheduleDeckReconnect(now - kDeckReconnectRetrySeconds, "");
    }

    if (now < g_nextDeckReconnectAt) {
        return;
    }

    tryConnectDeckBackend(false);
}

void maintainProfileSelection() {
    if (!g_profileSelectionPending) {
        return;
    }
    if (XPLMGetElapsedTime() < g_nextProfileSelectionAt) {
        return;
    }
    logInfo("profile", "Retrying deferred profile selection.");
    selectProfileForAircraft(false);
}

float flightLoopCallback(float inElapsedSinceLastCall, float inElapsedTimeSinceLastFlightLoop, int inCounter, void* inRefcon) {
    (void)inElapsedSinceLastCall;
    (void)inElapsedTimeSinceLastFlightLoop;
    (void)inCounter;
    (void)inRefcon;
    flushPendingLogEntries();
    processPendingKeyEvents();
    maintainPulseBindings();
    maintainDeckBackend();
    maintainProfileSelection();
    flushPendingLogEntries();
    return kFlightLoopInterval;
}

void registerFlightLoop() {
    if (g_flightLoopRegistered) {
        return;
    }
    XPLMRegisterFlightLoopCallback(flightLoopCallback, kFlightLoopInterval, nullptr);
    g_flightLoopRegistered = true;
    logDebug("lifecycle", "Flight loop registered.");
}

void unregisterFlightLoop() {
    if (!g_flightLoopRegistered) {
        return;
    }
    XPLMUnregisterFlightLoopCallback(flightLoopCallback, nullptr);
    g_flightLoopRegistered = false;
    logDebug("lifecycle", "Flight loop unregistered.");
}

int commandHandler(XPLMCommandRef inCommand, XPLMCommandPhase inPhase, void* inRefcon) {
    (void)inCommand;
    const auto which = decodeCommandRefcon(inRefcon);

    if (which == InternalCommand::ToggleWindow) {
        if (inPhase == xplm_CommandBegin) {
            logInfo("command", "Internal command invoked: toggle_window");
            toggleWindow();
        }
        return 1;
    }

    if (which == InternalCommand::ReloadPrefs) {
        if (inPhase == xplm_CommandBegin) {
            logInfo("command", "Internal command invoked: reload_prefs");
            reloadRuntimeConfig();
        }
        return 1;
    }

    if (which == InternalCommand::TestFirstBinding) {
        if (g_bindings.empty()) {
            g_statusLine = "No bindings available for test.";
            logWarn("command", g_statusLine);
            return 1;
        }

        ActionBinding& binding = g_bindings.front();
        if (inPhase == xplm_CommandBegin) {
            dispatchBinding(binding, true);
        } else if (inPhase == xplm_CommandEnd) {
            dispatchBinding(binding, false);
        }

        std::ostringstream msg;
        msg << "Tested key." << binding.keyIndex << " -> " << binding.commandPath << " (" << actionModeName(binding.mode) << ")";
        g_statusLine = msg.str();
        logInfo("command", g_statusLine);
        return 1;
    }

    return 0;
}

void drawTextLine(int x, int y, const std::string& line, float r = 1.0f, float g = 1.0f, float b = 1.0f) {
    float color[3] = {r, g, b};
    XPLMDrawString(color, x, y, line.c_str(), nullptr, xplmFont_Proportional);
}

std::string ellipsizeMiddle(const std::string& s, std::size_t maxLen) {
    if (s.size() <= maxLen || maxLen < 8) {
        return s;
    }
    const std::size_t side = (maxLen - 3) / 2;
    return s.substr(0, side) + "..." + s.substr(s.size() - side);
}

void drawWindow(XPLMWindowID inWindowID, void* inRefcon) {
    (void)inRefcon;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    XPLMGetWindowGeometry(inWindowID, &left, &top, &right, &bottom);

    XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
    XPLMDrawTranslucentDarkBox(left, top, right, bottom);

    const auto deck = g_deckBackend.currentDeck();
    const std::string deckStatus = g_deckBackend.statusLine();
    const int x = left + 18;
    int y = top - 34;
    const int step = 19;

    drawTextLine(x, y, std::string(PLUGIN_NAME) + " v" + PLUGIN_VERSION, 0.95f, 0.95f, 0.95f);
    y -= step;
    drawTextLine(x, y, "Status: " + g_statusLine, 0.85f, 0.92f, 1.0f);
    y -= step;
    drawTextLine(x, y, "Deck: " + deckStatus, 0.82f, 0.95f, 0.86f);
    y -= step;
    if (deck.connected) {
        drawTextLine(x, y, "Device: " + deck.product_name + (deck.serial.empty() ? std::string() : (" [" + deck.serial + "]")));
    } else {
        drawTextLine(x, y, "Device filter: " + (g_prefs.deckSerial.empty() ? std::string("<auto>") : g_prefs.deckSerial));
    }
    y -= step;
    drawTextLine(x, y, "Brightness: " + std::to_string(g_prefs.brightness) + "%  Auto-connect: " + std::string(g_prefs.hidAutoConnect ? "on" : "off"));
    y -= step;
    drawTextLine(x, y, "Deck images: " + boolOnOff(g_prefs.keyImagesEnabled) + "  Logging: file=" + boolOnOff(g_prefs.logfileEnabled) + " debug=" + boolOnOff(g_prefs.debugLogging));
    y -= step;
    drawTextLine(x, y, "Queued logs: " + std::to_string(pendingLogCount()));
    y -= step;
    drawTextLine(x, y, "Last key event: " + ellipsizeMiddle(g_lastKeyEventLine, 78));
    y -= step;
    drawTextLine(x, y, "Profile: " + g_activeProfileName + " [" + g_activeProfileSource + "] -> " + ellipsizeMiddle(pathToDisplay(g_paths.activeProfileFile), 58));
    y -= step;
    drawTextLine(x, y, "Bindings: " + std::to_string(g_bindings.size()) + " loaded, " + std::to_string(g_resolvedBindings) + " resolved");
    y -= step;
    drawTextLine(x, y, "Tailnum: " + g_lastKnownTailnum + "  Fallback: " + g_prefs.activeProfile);
    y -= step;
    drawTextLine(x, y, "Prefs: " + ellipsizeMiddle(pathToDisplay(g_paths.prefsFile), 84));
    y -= step * 2;
    drawTextLine(x, y, "Commands:", 1.0f, 0.9f, 0.7f);
    y -= step;
    drawTextLine(x + 12, y, std::string(PLUGIN_COMMAND_PREFIX) + "/toggle_window");
    y -= step;
    drawTextLine(x + 12, y, std::string(PLUGIN_COMMAND_PREFIX) + "/reload_prefs");
    y -= step;
    drawTextLine(x + 12, y, std::string(PLUGIN_COMMAND_PREFIX) + "/test_first_binding");
    y -= step * 2;
    drawTextLine(x, y, "Profile syntax: key.<index>=<command>|<mode>");
    y -= step;
    drawTextLine(x, y, "Profile syntax: label./key./bg./fg./accent.<index>=...");
    y -= step;
    drawTextLine(x, y, "Supported modes: once, hold, pulse");
}

int handleMouseClick(XPLMWindowID inWindowID, int x, int y, XPLMMouseStatus inMouse, void* inRefcon) {
    (void)inWindowID;
    (void)x;
    (void)y;
    (void)inMouse;
    (void)inRefcon;
    return 1;
}

void handleKey(XPLMWindowID inWindowID, char inKey, XPLMKeyFlags inFlags, char inVirtualKey, void* inRefcon, int losingFocus) {
    (void)inWindowID;
    (void)inKey;
    (void)inFlags;
    (void)inVirtualKey;
    (void)inRefcon;
    (void)losingFocus;
}

XPLMCursorStatus handleCursor(XPLMWindowID inWindowID, int x, int y, void* inRefcon) {
    (void)inWindowID;
    (void)x;
    (void)y;
    (void)inRefcon;
    return xplm_CursorArrow;
}

int handleMouseWheel(XPLMWindowID inWindowID, int x, int y, int wheel, int clicks, void* inRefcon) {
    (void)inWindowID;
    (void)x;
    (void)y;
    (void)wheel;
    (void)clicks;
    (void)inRefcon;
    return 1;
}

void createWindow() {
    if (g_window != nullptr) {
        return;
    }

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);

    XPLMCreateWindow_t params{};
    params.structSize = sizeof(params);
    params.left = left + 80;
    params.top = top - 80;
    params.right = params.left + kWindowWidth;
    params.bottom = params.top - kWindowHeight;
    params.visible = 0;
    params.drawWindowFunc = drawWindow;
    params.handleMouseClickFunc = handleMouseClick;
    params.handleKeyFunc = handleKey;
    params.handleCursorFunc = handleCursor;
    params.handleMouseWheelFunc = handleMouseWheel;
    params.refcon = nullptr;
    params.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    params.layer = xplm_WindowLayerFloatingWindows;
    params.handleRightClickFunc = handleMouseClick;

    g_window = XPLMCreateWindowEx(&params);
    if (g_window == nullptr) {
        logError("ui", "Failed to create plugin window");
        return;
    }

    XPLMSetWindowTitle(g_window, PLUGIN_MENU_TITLE);
    XPLMSetWindowResizingLimits(g_window, 460, 260, 1400, 900);
    XPLMSetWindowPositioningMode(g_window, xplm_WindowCenterOnMonitor, -1);
    applyWindowVisibility();
    logInfo("ui", "Status window created.");
}

void destroyWindow() {
    if (g_window != nullptr) {
        XPLMDestroyWindow(g_window);
        g_window = nullptr;
        logDebug("ui", "Status window destroyed.");
    }
}

void registerCommands() {
    const std::string prefix = PLUGIN_COMMAND_PREFIX;

    g_cmdToggleWindow = XPLMCreateCommand((prefix + "/toggle_window").c_str(), "Show or hide the XPStreamDeck status window");
    g_cmdReloadPrefs = XPLMCreateCommand((prefix + "/reload_prefs").c_str(), "Reload XPStreamDeck prefs and active profile");
    g_cmdTestFirstBinding = XPLMCreateCommand((prefix + "/test_first_binding").c_str(), "Trigger the first resolved XPStreamDeck binding");

    if (g_cmdToggleWindow == nullptr || g_cmdReloadPrefs == nullptr || g_cmdTestFirstBinding == nullptr) {
        logError("command", "Failed to create one or more internal commands.");
    }

    if (g_cmdToggleWindow != nullptr) {
        XPLMRegisterCommandHandler(g_cmdToggleWindow, commandHandler, 1, commandRefcon(InternalCommand::ToggleWindow));
    }
    if (g_cmdReloadPrefs != nullptr) {
        XPLMRegisterCommandHandler(g_cmdReloadPrefs, commandHandler, 1, commandRefcon(InternalCommand::ReloadPrefs));
    }
    if (g_cmdTestFirstBinding != nullptr) {
        XPLMRegisterCommandHandler(g_cmdTestFirstBinding, commandHandler, 1, commandRefcon(InternalCommand::TestFirstBinding));
    }
    logInfo("command", "Registered internal commands.");
}

void unregisterCommands() {
    if (g_cmdToggleWindow != nullptr) {
        XPLMUnregisterCommandHandler(g_cmdToggleWindow, commandHandler, 1, commandRefcon(InternalCommand::ToggleWindow));
    }
    if (g_cmdReloadPrefs != nullptr) {
        XPLMUnregisterCommandHandler(g_cmdReloadPrefs, commandHandler, 1, commandRefcon(InternalCommand::ReloadPrefs));
    }
    if (g_cmdTestFirstBinding != nullptr) {
        XPLMUnregisterCommandHandler(g_cmdTestFirstBinding, commandHandler, 1, commandRefcon(InternalCommand::TestFirstBinding));
    }
    logDebug("command", "Unregistered internal commands.");
}

void createMenu() {
    XPLMMenuID pluginsMenu = XPLMFindPluginsMenu();
    g_pluginsMenuItem = XPLMAppendMenuItem(pluginsMenu, PLUGIN_MENU_TITLE, nullptr, 0);
    g_menu = XPLMCreateMenu(PLUGIN_MENU_TITLE, pluginsMenu, g_pluginsMenuItem, nullptr, nullptr);
    if (g_menu == nullptr) {
        logError("ui", "Failed to create plugin menu");
        return;
    }

    XPLMAppendMenuItemWithCommand(g_menu, "Toggle Status Window", g_cmdToggleWindow);
    XPLMAppendMenuItemWithCommand(g_menu, "Reload Prefs", g_cmdReloadPrefs);
    XPLMAppendMenuItemWithCommand(g_menu, "Test First Binding", g_cmdTestFirstBinding);
    logInfo("ui", "Plugin menu created.");
}

void destroyMenu() {
    if (g_menu != nullptr) {
        XPLMDestroyMenu(g_menu);
        g_menu = nullptr;
        logDebug("ui", "Plugin menu destroyed.");
    }
}

} // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    g_mainThreadId = std::this_thread::get_id();
    std::snprintf(outName, 256, "%s", PLUGIN_NAME);
    std::snprintf(outSig, 256, "%s", PLUGIN_SIGNATURE);
    std::snprintf(outDesc, 256, "%s", PLUGIN_DESC);

    XPLMEnableFeature(kNativePathsFeature, 1);

    refreshBasePaths();
    loadPrefs();
    refreshActiveProfilePath();
    ensureDir(g_paths.pluginDir);
    ensureDir(g_paths.profilesDir);
    ensureDir(g_paths.logDir);
    writeDefaultProfileIfMissing();
    loadProfileCandidates();
    reopenLogFile();

    logInfo("lifecycle", "Starting " + std::string(PLUGIN_NAME) + " v" + PLUGIN_VERSION);
    logDebug("lifecycle", prefsSummary());
    logDebug("lifecycle",
        "plugin_dir=" + pathToDisplay(g_paths.pluginDir) +
        " prefs=" + pathToDisplay(g_paths.prefsFile) +
        " profiles=" + pathToDisplay(g_paths.profilesDir) +
        " log=" + pathToDisplay(g_paths.logFile));

    registerCommands();
    createMenu();
    createWindow();
    registerFlightLoop();

    selectProfileForAircraft(false);
    g_windowVisible = g_prefs.showWindowOnStart;
    applyWindowVisibility();

    return 1;
}

PLUGIN_API void XPluginStop(void) {
    logInfo("lifecycle", "Stopping plugin.");
    g_pluginCurrentlyEnabled = false;
    stopDeckBackend();
    releaseActiveBindings();
    flushPendingLogEntries();
    unregisterFlightLoop();
    destroyWindow();
    destroyMenu();
    unregisterCommands();
    flushPendingLogEntries();
    {
        std::lock_guard<std::mutex> guard(g_logMutex);
        if (g_logFile.is_open()) {
            g_logFile.close();
        }
    }
}

PLUGIN_API int XPluginEnable(void) {
    g_pluginCurrentlyEnabled = true;
    logInfo("lifecycle", "Plugin enabled.");
    startDeckBackend();
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    g_pluginCurrentlyEnabled = false;
    stopDeckBackend();
    releaseActiveBindings();
    flushPendingLogEntries();
    logInfo("lifecycle", "Plugin disabled.");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFromWho, int inMessage, void* inParam) {
    logDebug("message", "Received X-Plane message id=" + std::to_string(inMessage) + " from=" + std::to_string(inFromWho));
    if (inMessage == XPLM_MSG_PLANE_LOADED && inParam == nullptr) {
        logInfo("message", "Aircraft loaded, selecting profile by tailnum.");
        selectProfileForAircraft(true);
    }
}
