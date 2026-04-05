#include "core/TrafficLogger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <ctime>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")

using json = nlohmann::json;

// ── Helpers ───────────────────────────────────────────────────────────────────

std::wstring TrafficLogger::getAppDataDir() {
    wchar_t path[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path);
    return std::wstring(path) + L"\\NetMonitor";
}

bool TrafficLogger::ensureDirectory(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring TrafficLogger::getLogFilePath() {
    return getAppDataDir() + L"\\traffic_log.jsonl";
}

std::string TrafficLogger::wstrToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                  nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return {};
    std::string s(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                        &s[0], sz, nullptr, nullptr);
    return s;
}

std::wstring TrafficLogger::utf8ToWstr(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (sz <= 1) return {};
    std::wstring w(sz - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], sz);
    return w;
}

std::wstring TrafficLogger::fmtTimePoint(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    struct tm tm {};
    localtime_s(&tm, &t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// ── flushSession ──────────────────────────────────────────────────────────────

bool TrafficLogger::flushSession(
        const std::map<uint32_t, ProcessTrafficInfo>& snapshot,
        std::chrono::system_clock::time_point sessionStart) {

    if (snapshot.empty()) return true;

    auto sessionEnd = std::chrono::system_clock::now();
    int64_t durSec  = std::chrono::duration_cast<std::chrono::seconds>(
                          sessionEnd - sessionStart).count();

    if (!ensureDirectory(getAppDataDir())) return false;

    std::string logPath = wstrToUtf8(getLogFilePath());
    std::ofstream ofs(logPath, std::ios::app);
    if (!ofs) return false;

    std::string startStr = wstrToUtf8(fmtTimePoint(sessionStart));
    std::string endStr   = wstrToUtf8(fmtTimePoint(sessionEnd));

    for (const auto& [pid, info] : snapshot) {
        if (info.totalSent == 0 && info.totalRecv == 0) continue;
        json j;
        j["start"]    = startStr;
        j["end"]      = endStr;
        j["pid"]      = pid;
        j["name"]     = wstrToUtf8(info.processName);
        j["path"]     = wstrToUtf8(info.processPath);
        j["sent"]     = info.totalSent;
        j["recv"]     = info.totalRecv;
        j["duration"] = durSec;
        ofs << j.dump() << '\n';
    }
    return ofs.good();
}

// ── loadHistory ───────────────────────────────────────────────────────────────

std::vector<TrafficLogger::SessionEntry> TrafficLogger::loadHistory(int maxEntries) const {
    std::string logPath = wstrToUtf8(getLogFilePath());
    std::ifstream ifs(logPath);
    if (!ifs) return {};

    std::vector<SessionEntry> all;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            SessionEntry e;
            e.startTime       = utf8ToWstr(j.value("start",    std::string{}));
            e.endTime         = utf8ToWstr(j.value("end",      std::string{}));
            e.pid             = j.value("pid",      uint32_t(0));
            e.processName     = utf8ToWstr(j.value("name",     std::string{}));
            e.processPath     = utf8ToWstr(j.value("path",     std::string{}));
            e.totalSent       = j.value("sent",     uint64_t(0));
            e.totalRecv       = j.value("recv",     uint64_t(0));
            e.durationSeconds = j.value("duration", int64_t(0));
            all.push_back(std::move(e));
        } catch (...) {
            // Skip malformed lines
        }
    }

    // Trim to maxEntries (keep most recent), then reverse to newest-first
    if ((int)all.size() > maxEntries)
        all.erase(all.begin(), all.begin() + (int)(all.size() - maxEntries));
    std::reverse(all.begin(), all.end());
    return all;
}

// ── clearHistory ─────────────────────────────────────────────────────────────

bool TrafficLogger::clearHistory() const {
    std::string logPath = wstrToUtf8(getLogFilePath());
    std::ofstream ofs(logPath, std::ios::trunc);
    return ofs.good();
}
