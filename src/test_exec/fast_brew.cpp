#include <gattlib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bt/BLEDevice.hpp"
#include "bt/BLEHelper.hpp"
#include "bt/ByteEncDecoder.hpp"
#include "jutta_bt_proto/Utils.hpp"
#include "logger/Logger.hpp"

namespace {
// Relevant Jura UUIDs:
uuid_t P_MODE_UUID;
uuid_t START_PRODUCT_UUID;
uuid_t MACHINE_STATUS_UUID;

void init_uuids() {
    gattlib_string_to_uuid("5a401529-ab2e-2548-c435-08c300000710", 36, &P_MODE_UUID);
    gattlib_string_to_uuid("5a401525-ab2e-2548-c435-08c300000710", 36, &START_PRODUCT_UUID);
    gattlib_string_to_uuid("5a401524-ab2e-2548-c435-08c300000710", 36, &MACHINE_STATUS_UUID);
}

std::vector<uint8_t> encode_payload(const std::vector<uint8_t>& data, uint8_t key, bool overrideKey) {
    std::vector<uint8_t> encoded = data;
    encoded[0] = key;
    if (overrideKey && !encoded.empty()) {
        encoded[encoded.size() - 1] = key;
    }
    return bt::encDecBytes(encoded, key);
}
}  // namespace

int main(int argc, char* argv[]) {
    init_uuids();

    const char* envMac = std::getenv("JURA_MAC");
    std::string targetMac = (envMac && envMac[0] != '\0') ? envMac : "D6:C0:60:D9:0A:89";
    bool fireAndForget = false;
    bool dryRun = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--mac=", 0) == 0) {
            targetMac = arg.substr(6);
        } else if (arg == "--fire-and-forget" || arg == "-f") {
            fireAndForget = true;
        } else if (arg == "--dry-run" || arg == "-n") {
            dryRun = true;
        }
    }

    const auto t_start = std::chrono::steady_clock::now();
    SPDLOG_INFO("=== FastBrew: Targeting '{}' ===", targetMac);

    // Fast 1-packet sync: lock controller clock offset to peripheral
    bool canceled = false;
    std::shared_ptr<bt::ScanArgs> result = bt::scan_for_device("TT214H BlueFrog", &canceled, targetMac);
    if (!result) {
        SPDLOG_ERROR("Coffee maker not found.");
        return 1;
    }

    const auto t_found = std::chrono::steady_clock::now();
    const double scanSeconds = std::chrono::duration<double>(t_found - t_start).count();
    SPDLOG_INFO("Clock synchronized in {:.3f}s. Connecting...", scanSeconds);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::atomic<bool> brewCompleted{false};
    uint8_t encryptionKey = 0x2a; // Default known key

    bt::BLEDevice bleDevice(
        std::string{result->name},
        std::string{result->addr},
        [](const std::vector<uint8_t>&, const uuid_t&) {},
        []() {},
        []() {},
        [&](const std::vector<uint8_t>& data, const uuid_t& uuid) {
            if (gattlib_uuid_cmp(&uuid, &MACHINE_STATUS_UUID) == GATTLIB_SUCCESS) {
                std::vector<uint8_t> alertVec = bt::encDecBytes(data, encryptionKey);
                // Bit 35 = enjoy product
                if (alertVec.size() > 5) {
                    size_t offsetAbs = (35 >> 3) + 1;
                    size_t offsetByte = 7 - (35 & 0b111);
                    if (offsetAbs < alertVec.size() && ((alertVec[offsetAbs] >> offsetByte) & 0b1)) {
                        brewCompleted = true;
                    }
                }
            }
        });

    bool connected = false;
    for (int attempt = 1; attempt <= 5; attempt++) {
        SPDLOG_INFO("Connecting (attempt {}/5)...", attempt);
        if (bleDevice.connect()) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }

    if (!connected) {
        SPDLOG_ERROR("Failed to establish BLE connection after retries.");
        return 1;
    }

    const auto t_connected = std::chrono::steady_clock::now();
    const double connectSeconds = std::chrono::duration<double>(t_connected - t_start).count();
    SPDLOG_INFO("Connected in {:.3f}s from launch. Preparing command...", connectSeconds);

    // Extract dynamic key from advertisement if present:
    const std::vector<uint8_t>& manData = bleDevice.get_mam_data();
    if (!manData.empty()) {
        SPDLOG_INFO("[TRACK:ADV_DATA] Manufacturer data ({} bytes): {}", 
            manData.size(), jutta_bt_proto::to_hex_string(manData));
        if (manData.size() >= 16) {
            encryptionKey = manData[0];
        } else if (manData.size() >= 5) {
            encryptionKey = manData[4];
        }
    }
    SPDLOG_INFO("[TRACK:KEY] Dynamic Encryption Key: 0x{:02x} (dec {})", encryptionKey, encryptionKey);

    // 1. Keep-alive (stay_in_ble)
    static const std::vector<uint8_t> stayInBleCmd{0x00, 0x7F, 0x80};
    const std::vector<uint8_t> encodedStayInBle = encode_payload(stayInBleCmd, encryptionKey, false);
    SPDLOG_INFO("[TRACK:TX_KEEP_ALIVE] Raw: {} | Encoded: {}", 
        jutta_bt_proto::to_hex_string(stayInBleCmd), jutta_bt_proto::to_hex_string(encodedStayInBle));
    bleDevice.write_without_response(P_MODE_UUID, encodedStayInBle);

    // 2. Transmit 2 Espressi brew command (code 12, 100_0 hopper, 30ml water, high temp)
    const std::string rawCmdHex = "001200000600000200000000000000000000";
    const std::vector<uint8_t> rawCmd = jutta_bt_proto::from_hex_string(rawCmdHex);
    const std::vector<uint8_t> encodedCmd = encode_payload(rawCmd, encryptionKey, true);
    SPDLOG_INFO("[TRACK:TX_BREW_CMD] Raw: {} | Encoded: {}", 
        jutta_bt_proto::to_hex_string(rawCmd), jutta_bt_proto::to_hex_string(encodedCmd));

    if (dryRun) {
        const auto t_ready = std::chrono::steady_clock::now();
        const double readySeconds = std::chrono::duration<double>(t_ready - t_start).count();
        SPDLOG_INFO(">>> DRY-RUN: Machine connected, key loaded, keep-alive active in {:.3f}s! <<<", readySeconds);
        SPDLOG_INFO("Skipping command dispatch (dry-run). Disconnecting...");
        static const std::vector<uint8_t> disconnectCmd{0x00, 0x7F, 0x81};
        const std::vector<uint8_t> encodedDisconnect = encode_payload(disconnectCmd, encryptionKey, false);
        SPDLOG_INFO("[TRACK:TX_DISCONNECT] Raw: {} | Encoded: {}", 
            jutta_bt_proto::to_hex_string(disconnectCmd), jutta_bt_proto::to_hex_string(encodedDisconnect));
        bleDevice.write_without_response(P_MODE_UUID, encodedDisconnect);
        bleDevice.disconnect();
        return 0;
    }

    bool writeSuccess = bleDevice.write(START_PRODUCT_UUID, encodedCmd);
    const auto t_sent = std::chrono::steady_clock::now();
    const double totalSeconds = std::chrono::duration<double>(t_sent - t_start).count();

    if (!writeSuccess) {
        SPDLOG_ERROR("Failed to transmit brew command to machine.");
        bleDevice.disconnect();
        return 2;
    }

    SPDLOG_INFO(">>> BREW COMMAND DELIVERED & ACKNOWLEDGED in {:.3f}s total! <<<", totalSeconds);

    if (fireAndForget) {
        SPDLOG_INFO("Fire-and-forget enabled: disconnecting.");
        bleDevice.disconnect();
        return 0;
    }

    // Subscribe to status notifications to watch brew completion:
    bleDevice.subscribe(MACHINE_STATUS_UUID);
    SPDLOG_INFO("Monitoring brew progress (watching for 'enjoy product')...");

    const auto brewStart = std::chrono::steady_clock::now();
    while (!brewCompleted) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - brewStart).count() > 120.0) {
            SPDLOG_WARN("Brew monitoring timeout (120s reached).");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Send stay_in_ble heartbeat every 2 seconds:
        bleDevice.write_without_response(P_MODE_UUID, encode_payload(stayInBleCmd, encryptionKey, false));
    }

    if (brewCompleted) {
        SPDLOG_INFO("Saw 'enjoy product' alert - brew complete!");
    }

    // Clean disconnect:
    static const std::vector<uint8_t> disconnectCmd{0x00, 0x7F, 0x81};
    bleDevice.write_without_response(P_MODE_UUID, encode_payload(disconnectCmd, encryptionKey, false));
    bleDevice.disconnect();
    SPDLOG_INFO("Cleanly disconnected. Enjoy your coffee!");
    return 0;
}
