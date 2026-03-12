#include "plugin_config.h"

#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMUtilities.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "plugin_utils.h"

namespace {

namespace fs = std::filesystem;

constexpr const char* kNativePathsFeature = "XPLM_USE_NATIVE_PATHS";
constexpr int kWindowWidth = 620;
constexpr int kWindowHeight = 300;

enum class InternalCommand {
    ToggleWindow,
    ReloadPrefs,
    TestFirstBinding,
};

enum class ActionMode {
    Once,
    Hold,
};

struct Prefs {
    bool enabled = false;
    bool logfileEnabled = true;
    bool showWindowOnStart = false;
    std::string activeProfile = "default";
};

struct Paths {
    fs::path systemRoot;
    fs::path pluginDir;
    fs::path prefsFile;
    fs::path profilesDir;
    fs::path activeProfileFile;
    fs::path logDir;
    fs::path logFile;
};

struct ActionBinding {
    int keyIndex = -1;
    std::string commandPath;
    ActionMode mode = ActionMode::Once;
    XPLMCommandRef command = nullptr;
    bool active = false;
};

Prefs g_prefs;
Paths g_paths;
std::vector<ActionBinding> g_bindings;
std::ofstream g_logFile;
XPLMWindowID g_window = nullptr;
XPLMMenuID g_menu = nullptr;
int g_pluginsMenuItem = -1;
XPLMCommandRef g_cmdToggleWindow = nullptr;
XPLMCommandRef g_cmdReloadPrefs = nullptr;
XPLMCommandRef g_cmdTestFirstBinding = nullptr;
bool g_windowVisible = false;
int g_resolvedBindings = 0;
std::string g_statusLine = "Scaffold ready. HID backend pending.";
std::string g_lastProfileSummary = "No profile loaded yet.";

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

void logLine(const std::string& message) {
    const std::string full = "[" + nowString() + "] " + message;
    XPLMDebugString((std::string(PLUGIN_LOG_PREFIX) + ": " + full + "\n").c_str());
    if (g_logFile.is_open()) {
        g_logFile << full << '\n';
        g_logFile.flush();
    }
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
}

void refreshActiveProfilePath() {
    g_prefs.activeProfile = sanitizeProfileName(g_prefs.activeProfile);
    g_paths.activeProfileFile = g_paths.profilesDir / (g_prefs.activeProfile + ".cfg");
}

void ensureDir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        logLine("failed to create directory: " + pathToDisplay(dir) + " (" + ec.message() + ")");
    }
}

void writeDefaultProfileIfMissing() {
    if (fs::exists(g_paths.activeProfileFile)) {
        return;
    }
    ensureDir(g_paths.profilesDir);
    std::ofstream out(g_paths.activeProfileFile);
    if (!out.is_open()) {
        logLine("failed to create default profile: " + pathToDisplay(g_paths.activeProfileFile));
        return;
    }
    out
        << "# key.<index>=<command>|<mode>\n"
        << "# mode: once | hold\n"
        << '\n'
        << "key.0=sim/operation/pause_toggle|once\n"
        << "key.1=sim/flight_controls/flaps_down|once\n"
        << "key.2=sim/flight_controls/flaps_up|once\n"
        << "key.3=sim/flight_controls/brakes_toggle_max|once\n"
        << "key.4=sim/engines/thrust_reverse_hold|hold\n";
    logLine("created default profile: " + pathToDisplay(g_paths.activeProfileFile));
}

void savePrefs() {
    std::ofstream out(g_paths.prefsFile);
    if (!out.is_open()) {
        logLine("failed to write prefs: " + pathToDisplay(g_paths.prefsFile));
        return;
    }
    out
        << "enabled=" << bool01(g_prefs.enabled) << '\n'
        << "logfile_enabled=" << bool01(g_prefs.logfileEnabled) << '\n'
        << "show_window_on_start=" << bool01(g_prefs.showWindowOnStart) << '\n'
        << "active_profile=" << g_prefs.activeProfile << '\n';
}

void loadPrefs() {
    g_prefs = Prefs{};
    if (!fs::exists(g_paths.prefsFile)) {
        savePrefs();
        return;
    }

    std::ifstream in(g_paths.prefsFile);
    if (!in.is_open()) {
        logLine("failed to open prefs: " + pathToDisplay(g_paths.prefsFile));
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trimString(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::string key;
        std::string value;
        if (!splitOnce(line, '=', key, value)) {
            continue;
        }

        key = toLowerCopy(trimString(key));
        value = trimString(value);

        if (key == "enabled") {
            parseBool(value, g_prefs.enabled);
        } else if (key == "logfile_enabled") {
            parseBool(value, g_prefs.logfileEnabled);
        } else if (key == "show_window_on_start") {
            parseBool(value, g_prefs.showWindowOnStart);
        } else if (key == "active_profile") {
            g_prefs.activeProfile = value;
        }
    }
    g_prefs.activeProfile = sanitizeProfileName(g_prefs.activeProfile);
}

void reopenLogFile() {
    if (g_logFile.is_open()) {
        g_logFile.close();
    }
    if (!g_prefs.logfileEnabled) {
        return;
    }
    ensureDir(g_paths.logDir);
    g_logFile.open(g_paths.logFile, std::ios::app);
    if (!g_logFile.is_open()) {
        XPLMDebugString((std::string(PLUGIN_LOG_PREFIX) + ": failed to open log file\n").c_str());
    }
}

ActionMode parseActionMode(const std::string& raw) {
    if (toLowerCopy(trimString(raw)) == "hold") {
        return ActionMode::Hold;
    }
    return ActionMode::Once;
}

const char* actionModeName(ActionMode mode) {
    switch (mode) {
    case ActionMode::Hold:
        return "hold";
    case ActionMode::Once:
    default:
        return "once";
    }
}

void releaseHeldBindings() {
    for (auto& binding : g_bindings) {
        if (binding.active && binding.command != nullptr) {
            XPLMCommandEnd(binding.command);
            binding.active = false;
        }
    }
}

void resolveBindings() {
    g_resolvedBindings = 0;
    for (auto& binding : g_bindings) {
        binding.command = XPLMFindCommand(binding.commandPath.c_str());
        binding.active = false;
        if (binding.command != nullptr) {
            ++g_resolvedBindings;
        } else {
            logLine("unresolved command in profile: " + binding.commandPath);
        }
    }

    std::ostringstream summary;
    summary << "Loaded " << g_bindings.size() << " binding(s), resolved " << g_resolvedBindings << '.';
    g_lastProfileSummary = summary.str();
    g_statusLine = g_lastProfileSummary;
}

void loadProfile() {
    releaseHeldBindings();
    g_bindings.clear();
    writeDefaultProfileIfMissing();

    std::ifstream in(g_paths.activeProfileFile);
    if (!in.is_open()) {
        g_statusLine = "Failed to open active profile.";
        logLine("failed to open profile: " + pathToDisplay(g_paths.activeProfileFile));
        return;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trimString(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::string lhs;
        std::string rhs;
        if (!splitOnce(line, '=', lhs, rhs)) {
            continue;
        }

        lhs = toLowerCopy(trimString(lhs));
        rhs = trimString(rhs);

        if (lhs.rfind("key.", 0) != 0) {
            continue;
        }

        const std::string indexString = lhs.substr(4);
        if (indexString.empty()) {
            continue;
        }

        int keyIndex = -1;
        try {
            keyIndex = std::stoi(indexString);
        } catch (...) {
            logLine("invalid key index in profile: " + indexString);
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
            continue;
        }

        ActionBinding binding;
        binding.keyIndex = keyIndex;
        binding.commandPath = commandPath;
        binding.mode = parseActionMode(modeRaw);
        g_bindings.push_back(binding);
    }

    resolveBindings();
    logLine("active profile: " + g_prefs.activeProfile + " (" + pathToDisplay(g_paths.activeProfileFile) + ")");
}

void toggleWindow() {
    g_windowVisible = !g_windowVisible;
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
    refreshBasePaths();
    loadPrefs();
    refreshActiveProfilePath();
    ensureDir(g_paths.pluginDir);
    ensureDir(g_paths.profilesDir);
    ensureDir(g_paths.logDir);
    reopenLogFile();
    loadProfile();
    g_windowVisible = g_prefs.showWindowOnStart || g_windowVisible;
    applyWindowVisibility();
    logLine("prefs reloaded from " + pathToDisplay(g_paths.prefsFile));
}

void dispatchBinding(ActionBinding& binding, bool pressed) {
    if (binding.command == nullptr) {
        return;
    }

    switch (binding.mode) {
    case ActionMode::Once:
        if (pressed) {
            XPLMCommandOnce(binding.command);
        }
        break;
    case ActionMode::Hold:
        if (pressed) {
            if (!binding.active) {
                XPLMCommandBegin(binding.command);
                binding.active = true;
            }
        } else if (binding.active) {
            XPLMCommandEnd(binding.command);
            binding.active = false;
        }
        break;
    }
}

int commandHandler(XPLMCommandRef inCommand, XPLMCommandPhase inPhase, void* inRefcon) {
    (void)inCommand;
    const auto which = static_cast<InternalCommand>(reinterpret_cast<std::intptr_t>(inRefcon));

    if (which == InternalCommand::ToggleWindow) {
        if (inPhase == xplm_CommandBegin) {
            toggleWindow();
        }
        return 1;
    }

    if (which == InternalCommand::ReloadPrefs) {
        if (inPhase == xplm_CommandBegin) {
            reloadRuntimeConfig();
        }
        return 1;
    }

    if (which == InternalCommand::TestFirstBinding) {
        if (g_bindings.empty()) {
            g_statusLine = "No bindings available for test.";
            logLine(g_statusLine);
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

    const int x = left + 18;
    int y = top - 34;
    const int step = 19;

    drawTextLine(x, y, std::string(PLUGIN_NAME) + " v" + PLUGIN_VERSION, 0.95f, 0.95f, 0.95f);
    y -= step;
    drawTextLine(x, y, "Status: " + g_statusLine, 0.85f, 0.92f, 1.0f);
    y -= step;
    drawTextLine(x, y, "Backend: Stream Deck HID integration pending");
    y -= step;
    drawTextLine(x, y, "Prefs: " + ellipsizeMiddle(pathToDisplay(g_paths.prefsFile), 84));
    y -= step;
    drawTextLine(x, y, "Profile: " + g_prefs.activeProfile + " -> " + ellipsizeMiddle(pathToDisplay(g_paths.activeProfileFile), 72));
    y -= step;
    drawTextLine(x, y, "Bindings: " + std::to_string(g_bindings.size()) + " loaded, " + std::to_string(g_resolvedBindings) + " resolved");
    y -= step;
    drawTextLine(x, y, "Plugin enabled flag: " + std::string(g_prefs.enabled ? "on" : "off"));
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
    drawTextLine(x, y, "Supported modes: once, hold");
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
        logLine("failed to create plugin window");
        return;
    }

    XPLMSetWindowTitle(g_window, PLUGIN_MENU_TITLE);
    XPLMSetWindowResizingLimits(g_window, 420, 220, 1200, 800);
    XPLMSetWindowPositioningMode(g_window, xplm_WindowCenterOnMonitor, -1);
    applyWindowVisibility();
}

void destroyWindow() {
    if (g_window != nullptr) {
        XPLMDestroyWindow(g_window);
        g_window = nullptr;
    }
}

void registerCommands() {
    const std::string prefix = PLUGIN_COMMAND_PREFIX;

    g_cmdToggleWindow = XPLMCreateCommand((prefix + "/toggle_window").c_str(), "Show or hide the XPStreamDeck status window");
    g_cmdReloadPrefs = XPLMCreateCommand((prefix + "/reload_prefs").c_str(), "Reload XPStreamDeck prefs and active profile");
    g_cmdTestFirstBinding = XPLMCreateCommand((prefix + "/test_first_binding").c_str(), "Trigger the first resolved XPStreamDeck binding");

    XPLMRegisterCommandHandler(g_cmdToggleWindow, commandHandler, 1, reinterpret_cast<void*>(InternalCommand::ToggleWindow));
    XPLMRegisterCommandHandler(g_cmdReloadPrefs, commandHandler, 1, reinterpret_cast<void*>(InternalCommand::ReloadPrefs));
    XPLMRegisterCommandHandler(g_cmdTestFirstBinding, commandHandler, 1, reinterpret_cast<void*>(InternalCommand::TestFirstBinding));
}

void unregisterCommands() {
    if (g_cmdToggleWindow != nullptr) {
        XPLMUnregisterCommandHandler(g_cmdToggleWindow, commandHandler, 1, reinterpret_cast<void*>(InternalCommand::ToggleWindow));
    }
    if (g_cmdReloadPrefs != nullptr) {
        XPLMUnregisterCommandHandler(g_cmdReloadPrefs, commandHandler, 1, reinterpret_cast<void*>(InternalCommand::ReloadPrefs));
    }
    if (g_cmdTestFirstBinding != nullptr) {
        XPLMUnregisterCommandHandler(g_cmdTestFirstBinding, commandHandler, 1, reinterpret_cast<void*>(InternalCommand::TestFirstBinding));
    }
}

void createMenu() {
    XPLMMenuID pluginsMenu = XPLMFindPluginsMenu();
    g_pluginsMenuItem = XPLMAppendMenuItem(pluginsMenu, PLUGIN_MENU_TITLE, nullptr, 0);
    g_menu = XPLMCreateMenu(PLUGIN_MENU_TITLE, pluginsMenu, g_pluginsMenuItem, nullptr, nullptr);
    if (g_menu == nullptr) {
        logLine("failed to create plugin menu");
        return;
    }

    XPLMAppendMenuItemWithCommand(g_menu, "Toggle Status Window", g_cmdToggleWindow);
    XPLMAppendMenuItemWithCommand(g_menu, "Reload Prefs", g_cmdReloadPrefs);
    XPLMAppendMenuItemWithCommand(g_menu, "Test First Binding", g_cmdTestFirstBinding);
}

void destroyMenu() {
    if (g_menu != nullptr) {
        XPLMDestroyMenu(g_menu);
        g_menu = nullptr;
    }
}

} // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
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
    reopenLogFile();

    logLine("starting " + std::string(PLUGIN_NAME) + " v" + PLUGIN_VERSION);

    registerCommands();
    createMenu();
    createWindow();

    loadProfile();
    g_windowVisible = g_prefs.showWindowOnStart;
    applyWindowVisibility();

    return 1;
}

PLUGIN_API void XPluginStop(void) {
    logLine("stopping plugin");
    releaseHeldBindings();
    destroyWindow();
    destroyMenu();
    unregisterCommands();
    if (g_logFile.is_open()) {
        g_logFile.close();
    }
}

PLUGIN_API int XPluginEnable(void) {
    logLine("plugin enabled");
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    releaseHeldBindings();
    logLine("plugin disabled");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFromWho, int inMessage, void* inParam) {
    (void)inFromWho;
    if (inMessage == XPLM_MSG_PLANE_LOADED && inParam == nullptr) {
        logLine("aircraft loaded, re-resolving profile commands");
        loadProfile();
    }
}
