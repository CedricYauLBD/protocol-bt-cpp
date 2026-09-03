#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>

//---------------------------------------------------------------------------
namespace bt {
//---------------------------------------------------------------------------
struct ScanArgs {
    std::mutex doneMutex;
    std::mutex m;
    std::string name;
    std::regex nameRegex;
    std::string targetMac;
    bool success{false};
    std::string addr;
} __attribute__((aligned(128)));

std::shared_ptr<ScanArgs> scan_for_device(const std::string& regexStr, const bool* canceled, const std::string& targetMac = "");
//---------------------------------------------------------------------------
}  // namespace bt
//---------------------------------------------------------------------------