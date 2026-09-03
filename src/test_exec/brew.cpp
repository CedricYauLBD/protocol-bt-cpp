#include "bt/BLEHelper.hpp"
#include "jutta_bt_proto/CoffeeMaker.hpp"
#include "jutta_bt_proto/CoffeeMakerLoader.hpp"
#include "logger/Logger.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

namespace {

// CONFIRMED via a measured pour (30 units -> ~30ml, "2 Espressi" test brew) that raw
// WATER_AMOUNT units are ~1ml/unit on this machine. This contradicts the
// protocol-bt-cpp README's generic byte-layout doc, which says ~5ml/unit - that
// figure implies absurd totals for Coffee-range products (e.g. 750ml default for
// "2 Coffee") and should not be trusted for this machine.
constexpr double ML_PER_UNIT = 1.0;
constexpr double ML_PER_FL_OZ = 29.5735;

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string fmt_ml_oz(double ml) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(0) << ml << "ml (~" << std::setprecision(1) << (ml / ML_PER_FL_OZ) << "fl oz)";
    return oss.str();
}

void print_items_option(const std::string& label, const jutta_bt_proto::ItemsOption& opt) {
    std::cout << "  " << label << " (default: ";
    for (const jutta_bt_proto::Item& item : opt.items) {
        if (item.value == opt.defaultValue) {
            std::cout << item.name;
        }
    }
    std::cout << "):\n";
    for (const jutta_bt_proto::Item& item : opt.items) {
        std::cout << "    " << item.name << "\n";
    }
}

void print_min_max_option(const std::string& label, const jutta_bt_proto::MinMaxOption& opt) {
    std::cout << "  " << label << ": default=" << static_cast<int>(opt.value) << " units (" << fmt_ml_oz(opt.value * ML_PER_UNIT) << "), "
              << "range=" << static_cast<int>(opt.min) << "-" << static_cast<int>(opt.max) << " units ("
              << fmt_ml_oz(opt.min * ML_PER_UNIT) << " - " << fmt_ml_oz(opt.max * ML_PER_UNIT) << "), "
              << "step=" << static_cast<int>(opt.step) << " units (" << fmt_ml_oz(opt.step * ML_PER_UNIT) << ")\n";
}

void print_product_detail(const jutta_bt_proto::Product& p) {
    std::cout << "Options for '" << p.name << "' (code=" << p.code << "):\n";
    if (p.strength) print_items_option("strength", *p.strength);
    if (p.temperature) print_items_option("temperature", *p.temperature);
    if (p.waterAmount) print_min_max_option("water", *p.waterAmount);
    if (p.milkFoamAmount) print_min_max_option("milk foam", *p.milkFoamAmount);
    if (p.grinderRatio) print_items_option("hopper ratio", *p.grinderRatio);
    std::cout << "\nRun with --brew to actually brew, e.g.:\n"
              << "  ./jura_brew \"" << p.name << "\" --brew"
              << (p.grinderRatio ? " --hopper=<name-from-above>" : "")
              << (p.waterAmount ? " --water=<units-from-above>" : "") << "\n";
}

// Resolves a human-readable Item name (case-insensitive) to its raw hex Value.
std::optional<std::string> resolve_item_value(const jutta_bt_proto::ItemsOption& opt, const std::string& name) {
    const std::string wanted = to_lower(name);
    for (const jutta_bt_proto::Item& item : opt.items) {
        if (to_lower(item.name) == wanted) {
            return item.value;
        }
    }
    return std::nullopt;
}

struct CliOverrides {
    std::optional<std::string> hopperName;
    std::optional<uint8_t> waterUnits;
    std::optional<std::string> strengthName;
    std::optional<std::string> tempName;
    int count = 1;
};

// Resolves CLI overrides against a specific product's options. Returns nullopt (and
// logs the specific error) if any requested override doesn't apply to this product.
std::optional<jutta_bt_proto::Product::BrewOptions> resolve_brew_options(const jutta_bt_proto::Product& match, const CliOverrides& cli) {
    jutta_bt_proto::Product::BrewOptions options;
    bool optionError = false;

    if (cli.hopperName) {
        if (!match.grinderRatio) {
            SPDLOG_ERROR("'{}' has no hopper ratio option.", match.name);
            optionError = true;
        } else if (auto v = resolve_item_value(*match.grinderRatio, *cli.hopperName)) {
            options.grinderRatio = *v;
        } else {
            SPDLOG_ERROR("Unknown hopper ratio '{}'.", *cli.hopperName);
            optionError = true;
        }
    }
    if (cli.strengthName) {
        if (!match.strength) {
            SPDLOG_ERROR("'{}' has no strength option.", match.name);
            optionError = true;
        } else if (auto v = resolve_item_value(*match.strength, *cli.strengthName)) {
            options.strength = *v;
        } else {
            SPDLOG_ERROR("Unknown strength '{}'.", *cli.strengthName);
            optionError = true;
        }
    }
    if (cli.tempName) {
        if (!match.temperature) {
            SPDLOG_ERROR("'{}' has no temperature option.", match.name);
            optionError = true;
        } else if (auto v = resolve_item_value(*match.temperature, *cli.tempName)) {
            options.temperature = *v;
        } else {
            SPDLOG_ERROR("Unknown temperature '{}'.", *cli.tempName);
            optionError = true;
        }
    }
    if (cli.waterUnits) {
        if (!match.waterAmount) {
            SPDLOG_ERROR("'{}' has no water amount option.", match.name);
            optionError = true;
        } else if (*cli.waterUnits < match.waterAmount->min || *cli.waterUnits > match.waterAmount->max) {
            SPDLOG_ERROR("Water amount {} out of range [{}, {}].", *cli.waterUnits, match.waterAmount->min, match.waterAmount->max);
            optionError = true;
        } else {
            options.waterAmount = *cli.waterUnits;
        }
    }

    if (optionError) {
        return std::nullopt;
    }
    return options;
}

}  // namespace

// Usage:
//   ./jura_brew                                    List available products.
//   ./jura_brew "<product>"                        Show that product's configurable options (dry run, no brewing).
//   ./jura_brew "<product>" [--hopper=NAME] [--water=UNITS] [--strength=NAME] [--temp=NAME]
//                                                   Preview the exact raw command these overrides produce, still no brewing.
//   ./jura_brew "<product>" --brew [...same overrides...] [--count=N | --quattro] [--debug]
//                                                   Actually brew (repeated N times on same connection if requested).
//   --debug   Trace-level logging: prints every encoded byte string written to/read
//             from each BLE characteristic, in addition to the computed command.
int main(int argc, char** argv) {
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--debug") {
            debug = true;
        }
    }
    logger::setup_logger(debug ? spdlog::level::trace : spdlog::level::info);

    std::string wantedProduct;
    bool doBrew = false;
    CliOverrides cli;

    std::string targetMac = "";

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            // print_usage(argv[0]); // assuming this would be defined
            return 0;
        } else if (arg == "--brew") {
            doBrew = true;
        } else if (arg == "--debug") {
            // Already handled above.
        } else if (arg.rfind("--mac=", 0) == 0) {
            targetMac = arg.substr(6);
        } else if (arg.rfind("--hopper=", 0) == 0) {
            cli.hopperName = arg.substr(9);
        } else if (arg.rfind("--water=", 0) == 0) {
            cli.waterUnits = static_cast<uint8_t>(std::stoi(arg.substr(8)));
        } else if (arg.rfind("--strength=", 0) == 0) {
            cli.strengthName = arg.substr(11);
        } else if (arg.rfind("--temp=", 0) == 0) {
            cli.tempName = arg.substr(7);
        } else if (arg.rfind("--count=", 0) == 0) {
            cli.count = std::max(1, std::stoi(arg.substr(8)));
        } else if (arg == "--quattro") {
            cli.count = 2;
        } else if (wantedProduct.empty()) {
            wantedProduct = to_lower(arg);
        }
    }

    if (!targetMac.empty() && targetMac != "any") {
        SPDLOG_INFO("Targeting coffee maker MAC: {}", targetMac);
    }
    SPDLOG_INFO("Scanning for coffee maker...");
    bool canceled = false;
    std::shared_ptr<bt::ScanArgs> result = bt::scan_for_device("TT214H BlueFrog", &canceled, targetMac);
    if (!result) {
        SPDLOG_ERROR("No coffee maker found.");
        return 1;
    }

    // Allow BlueZ adapter to transition from active discovery to idle connect state:
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    jutta_bt_proto::CoffeeMaker coffeeMaker(std::string{result->name}, std::string{result->addr});

    std::shared_ptr<jutta_bt_proto::Joe> activeJoe;
    std::atomic<bool> handshakeComplete{false};

    coffeeMaker.joeChangedEventHandler.append([&](const std::shared_ptr<jutta_bt_proto::Joe>& joe) {
        activeJoe = joe;
        handshakeComplete = true;
    });

    if (!coffeeMaker.connect()) {
        SPDLOG_ERROR("Failed to connect.");
        return 1;
    }

    // Wait for the full handshake to finish (machine identification loaded & heartbeat started):
    SPDLOG_INFO("Waiting for initial BLE handshake and machine profile...");
    for (size_t i = 0; i < 100 && !handshakeComplete; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!handshakeComplete || !activeJoe) {
        SPDLOG_ERROR("Handshake timed out or machine profile not loaded.");
        coffeeMaker.disconnect();
        return 1;
    }
    SPDLOG_INFO("Handshake complete. Machine: '{}' Version: {}", activeJoe->machine->name, activeJoe->machine->version);

    if (wantedProduct.empty()) {
        std::cout << "Available products on '" << activeJoe->machine->name << "':\n";
        for (const jutta_bt_proto::Product& p : activeJoe->products) {
            std::cout << "  code=" << p.code << "  " << p.name << "\n";
        }
        coffeeMaker.disconnect();
        return 0;
    }

    const jutta_bt_proto::Product* match = nullptr;
    for (const jutta_bt_proto::Product& p : activeJoe->products) {
        if (to_lower(p.name).find(wantedProduct) != std::string::npos) {
            match = &p;
            break;
        }
    }

    if (!match) {
        SPDLOG_ERROR("No product matching '{}' found.", wantedProduct);
        coffeeMaker.disconnect();
        return 1;
    }

    const bool hasOverrides = cli.hopperName || cli.waterUnits || cli.strengthName || cli.tempName;

    if (!doBrew) {
        print_product_detail(*match);
        if (hasOverrides) {
            if (auto options = resolve_brew_options(*match, cli)) {
                std::cout << "\nPreview (not sent): raw command = " << match->to_bt_command(*options) << "\n";
            } else {
                coffeeMaker.disconnect();
                return 1;
            }
        }
        coffeeMaker.disconnect();
        return 0;
    }

    std::optional<jutta_bt_proto::Product::BrewOptions> options = resolve_brew_options(*match, cli);
    if (!options) {
        coffeeMaker.disconnect();
        return 3;
    }

    constexpr int EXIT_SUCCESS_CODE = 0;
    constexpr int EXIT_BREW_ABORTED_FAIL = 2;

    std::mutex alertMutex;
    std::optional<std::string> blockingAlertName;

    auto is_fatal_blocking_alert = [](const jutta_bt_proto::Alert& a) -> bool {
        const std::string nameLower = to_lower(a.name);
        if (nameLower == "heating up" || nameLower == "please wait" || nameLower == "system filling" || nameLower == "coffee rinsing") {
            return false;
        }
        if (a.type == "block") {
            return true;
        }
        if (nameLower.find("no beans") != std::string::npos ||
            nameLower.find("bean alert") != std::string::npos ||
            nameLower.find("fill water") != std::string::npos ||
            nameLower.find("empty grounds") != std::string::npos ||
            nameLower.find("empty tray") != std::string::npos ||
            nameLower.find("insert tray") != std::string::npos ||
            nameLower.find("error status") != std::string::npos) {
            return true;
        }
        return false;
    };

    std::atomic<bool> enjoySeen{false};
    activeJoe->alertsChangedEventHandler.append([&](const std::vector<const jutta_bt_proto::Alert*>& alerts) {
        std::ostringstream oss;
        bool first = true;
        bool hasEnjoy = false;
        for (const jutta_bt_proto::Alert* a : alerts) {
            if (!first) oss << ", ";
            first = false;
            oss << a->name;
            if (to_lower(a->name) == "enjoy product") {
                hasEnjoy = true;
            }
            if (is_fatal_blocking_alert(*a)) {
                std::lock_guard<std::mutex> lock(alertMutex);
                blockingAlertName = a->name;
            }
        }
        SPDLOG_INFO("Alerts now: [{}]", oss.str());
        if (hasEnjoy) {
            enjoySeen = true;
        }
    });

    const int count = cli.count;

    for (int cycle = 1; cycle <= count; cycle++) {
        if (count > 1) {
            SPDLOG_INFO("==================== Brew {}/{} ====================", cycle, count);
        }

        // Verify machine is not currently in a fatal blocked state before requesting brew:
        {
            std::lock_guard<std::mutex> lock(alertMutex);
            blockingAlertName.reset();
        }
        for (const auto* a : coffeeMaker.get_alerts()) {
            if (is_fatal_blocking_alert(*a)) {
                SPDLOG_ERROR("Machine is in blocked state '{}' before brew {}/{} — aborting.", a->name, cycle, count);
                coffeeMaker.disconnect();
                return EXIT_BREW_ABORTED_FAIL;
            }
        }

        enjoySeen = false;
        SPDLOG_INFO("Brewing '{}' (cycle {}/{}) with command: {}",
                    match->name, cycle, count, match->to_bt_command(*options));
        if (!coffeeMaker.request_coffee(*match, *options)) {
            SPDLOG_ERROR("Failed to write brew command for cycle {}/{} to BLE characteristic!", cycle, count);
            coffeeMaker.disconnect();
            return EXIT_BREW_ABORTED_FAIL;
        }

        // Wait up to 120s for completion ('enjoy product')
        constexpr int MAX_WAIT_SECONDS = 120;
        SPDLOG_INFO("Waiting up to {}s for brew {}/{} to complete (watching for 'enjoy product')...",
                    MAX_WAIT_SECONDS, cycle, count);
        bool sawEnjoyThisCycle = false;
        bool brewFailed = false;
        for (int i = 0; i < MAX_WAIT_SECONDS; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            {
                std::lock_guard<std::mutex> lock(alertMutex);
                if (blockingAlertName) {
                    SPDLOG_ERROR("Machine reported blocking alert '{}' during brew {}/{} — aborting!", *blockingAlertName, cycle, count);
                    brewFailed = true;
                    break;
                }
            }
            if (enjoySeen) {
                sawEnjoyThisCycle = true;
                break;
            }
            if (coffeeMaker.get_state() != jutta_bt_proto::CoffeeMakerState::CONNECTED) {
                SPDLOG_ERROR("BLE connection lost during brew {}/{}!", cycle, count);
                brewFailed = true;
                break;
            }
        }

        if (brewFailed || !sawEnjoyThisCycle) {
            if (!brewFailed) {
                SPDLOG_WARN("Never saw 'enjoy product' within {}s for brew {}/{} - aborting.",
                            MAX_WAIT_SECONDS, cycle, count);
            }
            coffeeMaker.disconnect();
            return EXIT_BREW_ABORTED_FAIL;
        }
        SPDLOG_INFO("Saw 'enjoy product' - brew {}/{} completed!", cycle, count);

        // If there is another brew coming, wait for machine to clear 'enjoy product' and return to idle
        if (cycle < count) {
            SPDLOG_INFO("Brew {}/{} done. Waiting for machine to clear 'enjoy product' before next brew...", cycle, count);
            for (int i = 0; i < 20; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                bool stillEnjoy = false;
                for (const auto* a : coffeeMaker.get_alerts()) {
                    if (to_lower(a->name) == "enjoy product") {
                        stillEnjoy = true;
                        break;
                    }
                }
                if (!stillEnjoy) {
                    SPDLOG_INFO("Machine cleared 'enjoy product' alert.");
                    break;
                }
            }
            constexpr int SETTLE_SECONDS = 3;
            SPDLOG_INFO("Pausing {}s for mechanical reset before cycle {}/{}...", SETTLE_SECONDS, cycle + 1, count);
            std::this_thread::sleep_for(std::chrono::seconds(SETTLE_SECONDS));
        }
    }

    coffeeMaker.disconnect();
    return EXIT_SUCCESS_CODE;
}
