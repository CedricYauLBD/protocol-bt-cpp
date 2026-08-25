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
//   ./jura_brew "<product>" --brew [...same overrides...] [--debug]
//                                                   Actually brew.
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

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--brew") {
            doBrew = true;
        } else if (arg == "--debug") {
            // Already handled above.
        } else if (arg.rfind("--hopper=", 0) == 0) {
            cli.hopperName = arg.substr(9);
        } else if (arg.rfind("--water=", 0) == 0) {
            cli.waterUnits = static_cast<uint8_t>(std::stoi(arg.substr(8)));
        } else if (arg.rfind("--strength=", 0) == 0) {
            cli.strengthName = arg.substr(11);
        } else if (arg.rfind("--temp=", 0) == 0) {
            cli.tempName = arg.substr(7);
        } else if (wantedProduct.empty()) {
            wantedProduct = to_lower(arg);
        }
    }

    SPDLOG_INFO("Scanning for coffee maker...");
    bool canceled = false;
    std::shared_ptr<bt::ScanArgs> result = bt::scan_for_device("TT214H BlueFrog", &canceled);
    if (!result) {
        SPDLOG_ERROR("No coffee maker found.");
        return 1;
    }

    jutta_bt_proto::CoffeeMaker coffeeMaker(std::string{result->name}, std::string{result->addr});

    std::atomic<bool> ready{false};
    std::atomic<bool> brewed{false};
    std::atomic<bool> failed{false};
    // UNCONFIRMED completion signal: bit 31 in this machine's ALERTS table is named
    // "enjoy product", which reads like Jura's classic "Enjoy your coffee!" message -
    // a much better completion candidate than "coffee ready" (bit 13), which is
    // misleading: it's already set at idle, before ever brewing anything, so it more
    // likely means "system ready to brew" than "your drink is done". Not yet verified
    // against a real timed brew - alert changes are logged live below so this can be
    // confirmed/corrected by watching them against the actual machine.
    std::atomic<bool> brewComplete{false};
    coffeeMaker.joeChangedEventHandler.append([&](const std::shared_ptr<jutta_bt_proto::Joe>& joe) {
        if (wantedProduct.empty()) {
            std::cout << "Available products on '" << joe->machine->name << "':\n";
            for (const jutta_bt_proto::Product& p : joe->products) {
                std::cout << "  code=" << p.code << "  " << p.name << "\n";
            }
            ready = true;
            return;
        }

        const jutta_bt_proto::Product* match = nullptr;
        for (const jutta_bt_proto::Product& p : joe->products) {
            if (to_lower(p.name).find(wantedProduct) != std::string::npos) {
                match = &p;
                break;
            }
        }

        if (!match) {
            SPDLOG_ERROR("No product matching '{}' found.", wantedProduct);
            failed = true;
            ready = true;
            return;
        }

        const bool hasOverrides = cli.hopperName || cli.waterUnits || cli.strengthName || cli.tempName;

        if (!doBrew) {
            print_product_detail(*match);
            if (hasOverrides) {
                if (auto options = resolve_brew_options(*match, cli)) {
                    std::cout << "\nPreview (not sent): raw command = " << match->to_bt_command(*options) << "\n";
                } else {
                    failed = true;
                }
            }
            ready = true;
            return;
        }

        std::optional<jutta_bt_proto::Product::BrewOptions> options = resolve_brew_options(*match, cli);
        if (!options) {
            failed = true;
            ready = true;
            return;
        }

        // Log every alert change live, and watch for the completion candidate.
        joe->alertsChangedEventHandler.append([&](const std::vector<const jutta_bt_proto::Alert*>& alerts) {
            std::ostringstream oss;
            bool first = true;
            for (const jutta_bt_proto::Alert* a : alerts) {
                if (!first) oss << ", ";
                first = false;
                oss << a->name;
                if (to_lower(a->name) == "enjoy product") {
                    brewComplete = true;
                }
            }
            SPDLOG_INFO("Alerts now: [{}]", oss.str());
        });

        SPDLOG_INFO("Brewing '{}' with command: {}", match->name, match->to_bt_command(*options));
        coffeeMaker.request_coffee(*match, *options);
        brewed = true;
        ready = true;
    });

    if (!coffeeMaker.connect()) {
        SPDLOG_ERROR("Failed to connect.");
        return 1;
    }

    // Wait for the machine identification (which carries the product list) and,
    // if requested, for the brew command to have been sent.
    for (size_t i = 0; i < 100 && !ready; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Wait for the completion signal (see brewComplete comment above), with a
    // generous safety timeout in case that signal turns out to be wrong.
    if (brewed) {
        constexpr int MAX_WAIT_SECONDS = 120;
        SPDLOG_INFO("Waiting up to {}s for the machine to finish (watching for 'enjoy product')...", MAX_WAIT_SECONDS);
        for (int i = 0; i < MAX_WAIT_SECONDS && !brewComplete; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (brewComplete) {
            SPDLOG_INFO("Saw 'enjoy product' - treating as done.");
        } else {
            SPDLOG_WARN("Never saw 'enjoy product' within {}s - disconnecting anyway. Check the alert log above.", MAX_WAIT_SECONDS);
        }
    }

    coffeeMaker.disconnect();
    return failed ? 1 : 0;
}
