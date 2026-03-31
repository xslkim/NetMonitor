#include "core/WfpLimiter.h"
#include <fwpmtypes.h>
#include <fwpvi.h>

#pragma comment(lib, "fwpuclnt.lib")

// {A7F4E3B1-C9D2-4E5F-8A1B-3D6C7E9F2A4B}
static const GUID NETMONITOR_SUBLAYER_GUID =
    {0xA7F4E3B1, 0xC9D2, 0x4E5F, {0x8A, 0x1B, 0x3D, 0x6C, 0x7E, 0x9F, 0x2A, 0x4B}};

WfpLimiter::WfpLimiter() {
    sublayerGuid_ = NETMONITOR_SUBLAYER_GUID;
}

WfpLimiter::~WfpLimiter() {
    cleanup();
}

bool WfpLimiter::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (engine_) return true;

    DWORD result = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, nullptr, &engine_);
    if (result != ERROR_SUCCESS) {
        lastError_ = L"FwpmEngineOpen failed: " + std::to_wstring(result);
        return false;
    }

    // Add sublayer
    FWPM_SUBLAYER0 sublayer = {};
    sublayer.subLayerKey = sublayerGuid_;
    sublayer.displayData.name = const_cast<wchar_t*>(L"NetMonitor Limiter");
    sublayer.displayData.description = const_cast<wchar_t*>(L"NetMonitor bandwidth limiter sublayer");
    sublayer.weight = 0x100;

    result = FwpmSubLayerAdd0(engine_, &sublayer, nullptr);
    if (result != ERROR_SUCCESS && result != FWP_E_ALREADY_EXISTS) {
        lastError_ = L"FwpmSubLayerAdd failed: " + std::to_wstring(result);
        FwpmEngineClose0(engine_);
        engine_ = nullptr;
        return false;
    }

    return true;
}

void WfpLimiter::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) return;

    // Remove all active filters
    for (auto& [key, entry] : filters_) {
        if (entry.active) {
            FwpmFilterDeleteById0(engine_, entry.filterId);
        }
    }
    filters_.clear();

    // Remove sublayer
    FwpmSubLayerDeleteByKey0(engine_, &sublayerGuid_);

    FwpmEngineClose0(engine_);
    engine_ = nullptr;
}

bool WfpLimiter::blockProcess(uint32_t pid, const std::wstring& processPath, Direction dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) return false;

    auto key = makeKey(pid, dir);
    if (filters_.count(key) && filters_[key].active) return true; // already blocked

    // Get app ID from path
    FWP_BYTE_BLOB* appId = nullptr;
    DWORD result = FwpmGetAppIdFromFileName0(processPath.c_str(), &appId);
    if (result != ERROR_SUCCESS) {
        lastError_ = L"FwpmGetAppIdFromFileName failed: " + std::to_wstring(result);
        return false;
    }

    // Build filter condition: match on app ID
    FWPM_FILTER_CONDITION0 cond = {};
    cond.fieldKey = FWPM_CONDITION_ALE_APP_ID;
    cond.matchType = FWP_MATCH_EQUAL;
    cond.conditionValue.type = FWP_BYTE_BLOB_TYPE;
    cond.conditionValue.byteBlob = appId;

    // Choose layer based on direction
    const GUID* layerKey = (dir == Direction::Upload)
        ? &FWPM_LAYER_OUTBOUND_TRANSPORT_V4
        : &FWPM_LAYER_INBOUND_TRANSPORT_V4;

    // Build filter
    FWPM_FILTER0 filter = {};
    filter.layerKey = *layerKey;
    filter.subLayerKey = sublayerGuid_;
    filter.displayData.name = const_cast<wchar_t*>(L"NetMonitor Block");
    filter.weight.type = FWP_UINT8;
    filter.weight.uint8 = 15;
    filter.numFilterConditions = 1;
    filter.filterCondition = &cond;
    filter.action.type = FWP_ACTION_BLOCK;

    UINT64 filterId = 0;
    result = FwpmFilterAdd0(engine_, &filter, nullptr, &filterId);
    FwpmFreeMemory0(reinterpret_cast<void**>(&appId));

    if (result != ERROR_SUCCESS) {
        lastError_ = L"FwpmFilterAdd failed: " + std::to_wstring(result);
        return false;
    }

    filters_[key] = {filterId, true};
    return true;
}

bool WfpLimiter::unblockProcess(uint32_t pid, Direction dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) return false;

    auto key = makeKey(pid, dir);
    auto it = filters_.find(key);
    if (it == filters_.end() || !it->second.active) return true;

    DWORD result = FwpmFilterDeleteById0(engine_, it->second.filterId);
    if (result != ERROR_SUCCESS && result != FWP_E_FILTER_NOT_FOUND) {
        lastError_ = L"FwpmFilterDeleteById failed: " + std::to_wstring(result);
        return false;
    }

    it->second.active = false;
    filters_.erase(it);
    return true;
}

bool WfpLimiter::isBlocked(uint32_t pid, Direction dir) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = makeKey(pid, dir);
    auto it = filters_.find(key);
    return it != filters_.end() && it->second.active;
}
