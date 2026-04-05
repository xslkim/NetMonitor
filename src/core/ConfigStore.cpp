#include "core/ConfigStore.h"
#include <nlohmann/json.hpp>
#include <fstream>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")

using json = nlohmann::json;

// ── Helpers ───────────────────────────────────────────────────────────────────

std::wstring ConfigStore::getAppDataDir() {
    wchar_t path[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path);
    return std::wstring(path) + L"\\NetMonitor";
}

std::wstring ConfigStore::getConfigFilePath() {
    return getAppDataDir() + L"\\config.json";
}

std::string ConfigStore::wstrToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return {};
    std::string s(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                        &s[0], sz, nullptr, nullptr);
    return s;
}

std::wstring ConfigStore::utf8ToWstr(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (sz <= 1) return {};
    std::wstring w(sz - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], sz);
    return w;
}

// ── save ─────────────────────────────────────────────────────────────────────

bool ConfigStore::save(const Config& cfg) const {
    // Ensure directory exists
    std::wstring dir = getAppDataDir();
    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) return false;

    json root;
    root["version"] = 1;

    // Limits (skip entries where both limits are 0)
    json limArr = json::array();
    for (const auto& e : cfg.limits) {
        if (e.sendLimit == 0 && e.recvLimit == 0) continue;
        json entry;
        entry["path"]       = wstrToUtf8(e.processPath);
        entry["name"]       = wstrToUtf8(e.processName);
        entry["send_limit"] = e.sendLimit;
        entry["recv_limit"] = e.recvLimit;
        limArr.push_back(std::move(entry));
    }
    root["limits"] = limArr;

    // Alert policies (pid is runtime-only, not persisted)
    json polArr = json::array();
    for (const auto& p : cfg.alertPolicies) {
        json entry;
        entry["process_name"]        = wstrToUtf8(p.processName);
        entry["threshold_bytes"]     = p.thresholdBytes;
        entry["window_seconds"]      = p.windowSeconds;
        entry["direction"]           = (p.direction == Direction::Download)
                                           ? "download" : "upload";
        entry["limit_bytes_per_sec"] = p.limitBytesPerSec;
        polArr.push_back(std::move(entry));
    }
    root["alert_policies"] = polArr;

    std::string cfgPath = wstrToUtf8(getConfigFilePath());
    std::ofstream ofs(cfgPath);
    if (!ofs) return false;
    ofs << root.dump(2);
    return ofs.good();
}

// ── load ─────────────────────────────────────────────────────────────────────

bool ConfigStore::load(Config& outCfg) const {
    std::string cfgPath = wstrToUtf8(getConfigFilePath());
    std::ifstream ifs(cfgPath);
    if (!ifs) return false;

    try {
        json root = json::parse(ifs);

        if (root.contains("limits")) {
            for (const auto& e : root["limits"]) {
                LimitEntry le;
                le.processPath = utf8ToWstr(e.value("path",       std::string{}));
                le.processName = utf8ToWstr(e.value("name",       std::string{}));
                le.sendLimit   = e.value("send_limit", uint64_t(0));
                le.recvLimit   = e.value("recv_limit", uint64_t(0));
                if (!le.processPath.empty() &&
                    (le.sendLimit > 0 || le.recvLimit > 0))
                    outCfg.limits.push_back(std::move(le));
            }
        }

        if (root.contains("alert_policies")) {
            for (const auto& e : root["alert_policies"]) {
                AlertPolicy p;
                p.pid              = 0; // will be reconnected at runtime
                p.processName      = utf8ToWstr(e.value("process_name", std::string{}));
                p.thresholdBytes   = e.value("threshold_bytes",     uint64_t(0));
                p.windowSeconds    = e.value("window_seconds",       0);
                p.limitBytesPerSec = e.value("limit_bytes_per_sec", uint64_t(0));
                p.triggered        = false;
                std::string dir    = e.value("direction", std::string("download"));
                p.direction = (dir == "upload") ? Direction::Upload : Direction::Download;
                if (!p.processName.empty() && p.thresholdBytes > 0)
                    outCfg.alertPolicies.push_back(std::move(p));
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}
