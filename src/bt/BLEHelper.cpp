#include "bt/BLEHelper.hpp"
#include <chrono>
#include <logger/Logger.hpp>
#include <memory>
#include <optional>
#include <regex>
#include <thread>
#include <gattlib.h>
#include <spdlog/spdlog.h>

//---------------------------------------------------------------------------
namespace bt {
//---------------------------------------------------------------------------
void on_device_discovered(void* adapter, const char* addr, const char* name, void* userData) {
    ScanArgs* args = static_cast<ScanArgs*>(userData);
    args->m.lock();
    bool match = false;
    if (addr && !args->targetMac.empty() && args->targetMac != "any") {
        std::string a(addr);
        std::string t(args->targetMac);
        std::transform(a.begin(), a.end(), a.begin(), ::toupper);
        std::transform(t.begin(), t.end(), t.begin(), ::toupper);
        if (a == t) {
            match = true;
        }
    } else if (name && std::regex_match(name, args->nameRegex)) {
        match = true;
    }

    if (match) {
        args->success = true;
        args->name = name ? name : "Unknown";
        args->addr = addr ? addr : "";
        gattlib_adapter_scan_disable(adapter);
        SPDLOG_INFO("Coffee maker found: '{}' ({})", args->name, args->addr);
        args->doneMutex.unlock();
    }
    args->m.unlock();
    if (name && addr) {
        SPDLOG_DEBUG("FOUND: {} ({})", name, addr);
    }
}

std::shared_ptr<ScanArgs> scan_for_device(const std::string& regexStr, const bool* canceled, const std::string& targetMac) {
    SPDLOG_DEBUG("Scanning for devices...");
    void* adapter = nullptr;
    int result = gattlib_adapter_open(nullptr, &adapter);
    if (result != GATTLIB_SUCCESS) {
        SPDLOG_ERROR("Failed to open Bluetooth adapter with error code {}.", result);
        return nullptr;
    }

    std::shared_ptr<ScanArgs> args = std::make_shared<ScanArgs>();
    args->nameRegex = std::regex(regexStr);
    args->targetMac = targetMac;
    args->doneMutex.lock();

    size_t timeoutSeconds = 0;
    if (gattlib_adapter_scan_enable(adapter, &on_device_discovered, timeoutSeconds, args.get())) {
        SPDLOG_ERROR("Bluetooth scan failed.");
        gattlib_adapter_close(adapter);
        return nullptr;
    }
    const auto startTime = std::chrono::steady_clock::now();
    constexpr std::chrono::seconds SCAN_TIMEOUT{10};
    while (true) {
        if (args->doneMutex.try_lock()) {
            args->doneMutex.unlock();
            break;
        }
        if (*canceled || (std::chrono::steady_clock::now() - startTime >= SCAN_TIMEOUT)) {
            if (*canceled) {
                SPDLOG_DEBUG("Stopping scan (canceled)...");
            } else {
                SPDLOG_WARN("Bluetooth scan timed out after {}s without finding coffee maker.", SCAN_TIMEOUT.count());
            }
            gattlib_adapter_scan_disable(adapter);
            args->doneMutex.unlock();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    gattlib_adapter_close(adapter);
    SPDLOG_INFO("Scan stoped");
    if (args->success) {
        return args;
    }
    return nullptr;
}
//---------------------------------------------------------------------------
}  // namespace bt
//---------------------------------------------------------------------------