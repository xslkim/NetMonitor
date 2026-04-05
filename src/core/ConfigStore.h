#pragma once

#include "core/Types.h"
#include <string>
#include <vector>
#include <cstdint>

// ── ConfigStore ───────────────────────────────────────────────────────────────
// Persists per-process rate limits and alert policies to
// %APPDATA%\NetMonitor\config.json across sessions.
// Limits are keyed by processPath (not PID) so they survive restarts.

class ConfigStore {
public:
    struct LimitEntry {
        std::wstring processPath;
        std::wstring processName;
        uint64_t sendLimit = 0;  // bytes/sec, 0 = no limit
        uint64_t recvLimit = 0;
    };

    struct Config {
        std::vector<LimitEntry>  limits;
        std::vector<AlertPolicy> alertPolicies; // pid field is 0; reconnected at runtime
    };

    // Overwrite config.json with current state. Returns false on I/O error.
    bool save(const Config& cfg) const;

    // Load config.json. Returns false if missing or malformed (first-run is normal).
    bool load(Config& outCfg) const;

    static std::wstring getConfigFilePath();

private:
    static std::wstring getAppDataDir();
    static std::string  wstrToUtf8(const std::wstring& w);
    static std::wstring utf8ToWstr(const std::string& s);
};
