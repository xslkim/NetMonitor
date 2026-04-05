#pragma once

#include "core/Types.h"
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <cstdint>

// ── TrafficLogger ─────────────────────────────────────────────────────────────
// Records per-process traffic totals at session end (append-only JSON Lines).
// Each call to flushSession() appends one line per process to the log file.
// loadHistory() reads the file and returns entries newest-first.

class TrafficLogger {
public:
    struct SessionEntry {
        std::wstring startTime;       // "2026-04-05T14:30:00"
        std::wstring endTime;
        uint32_t     pid = 0;
        std::wstring processName;
        std::wstring processPath;
        uint64_t     totalSent = 0;   // bytes
        uint64_t     totalRecv = 0;
        int64_t      durationSeconds = 0;
    };

    // Write this session's data to the log file.
    // Skips processes with zero traffic. Returns false on I/O error.
    bool flushSession(const std::map<uint32_t, ProcessTrafficInfo>& snapshot,
                      std::chrono::system_clock::time_point sessionStart);

    // Read history, newest first. maxEntries caps the result size.
    std::vector<SessionEntry> loadHistory(int maxEntries = 2000) const;

    // Truncate the log file.
    bool clearHistory() const;

    static std::wstring getLogFilePath();

private:
    static std::wstring getAppDataDir();
    static bool         ensureDirectory(const std::wstring& dir);
    static std::string  wstrToUtf8(const std::wstring& w);
    static std::wstring utf8ToWstr(const std::string& s);
    static std::wstring fmtTimePoint(std::chrono::system_clock::time_point tp);
};
