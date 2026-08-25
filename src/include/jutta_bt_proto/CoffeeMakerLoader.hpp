#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <eventpp/callbacklist.h>

//---------------------------------------------------------------------------
namespace jutta_bt_proto {
//---------------------------------------------------------------------------
struct Machine {
    size_t articleNumber;
    std::string name;
    std::string fileName;
    uint8_t version;

    Machine(size_t articleNumber, std::string&& name, std::string&& fileName, uint8_t version) : articleNumber(articleNumber),
                                                                                                 name(std::move(name)),
                                                                                                 fileName(std::move(fileName)),
                                                                                                 version(version){};
} __attribute__((aligned(128)));

struct Item {
    std::string name;
    std::string value;

    Item(std::string&& name, std::string&& value) : name(std::move(name)),
                                                    value(std::move(value)) {}
} __attribute__((aligned(64)));

struct ItemsOption {
    std::string argument;
    std::string defaultValue;
    std::vector<Item> items;

    ItemsOption(std::string&& argument, std::string&& defaultValue, std::vector<Item>&& items) : argument(std::move(argument)),
                                                                                                 defaultValue(std::move(defaultValue)),
                                                                                                 items(std::move(items)) {}

    /**
     * Writes this option's value into the command. If `override` is set, its raw
     * hex Value (as found in the ITEM list) is used instead of defaultValue.
     **/
    void to_bt_command(std::vector<std::string>& command, const std::optional<std::string>& override = std::nullopt) const;
} __attribute__((aligned(128)));

struct MinMaxOption {
    std::string argument;
    uint8_t value;
    uint8_t min;
    uint8_t max;
    uint8_t step;

    MinMaxOption(std::string&& argument, uint8_t value, uint8_t min, uint8_t max, uint8_t step) : argument(std::move(argument)),
                                                                                                  value(value),
                                                                                                  min(min),
                                                                                                  max(max),
                                                                                                  step(step) {}

    /**
     * Writes this option's value into the command. If `override` is set, it is used
     * (in the same raw units as value/min/max/step) instead of the default `value`.
     **/
    void to_bt_command(std::vector<std::string>& command, std::optional<uint8_t> override = std::nullopt) const;
} __attribute__((aligned(64)));

struct Product {
    std::string name;
    std::string code;

    std::optional<ItemsOption> strength;
    std::optional<ItemsOption> temperature;
    std::optional<MinMaxOption> waterAmount;
    std::optional<MinMaxOption> milkFoamAmount;
    std::optional<ItemsOption> grinderRatio;

    size_t statCounter{0};

    /**
     * Per-brew overrides. Any field left unset falls back to that option's own
     * default from the machine file. Strength/temperature/grinderRatio take the
     * raw hex Value string from the option's ITEM list (not the human-readable Name).
     * waterAmount/milkFoamAmount take a raw value in the same units as that
     * option's min/max/step (NOT milliliters).
     **/
    struct BrewOptions {
        std::optional<std::string> strength;
        std::optional<std::string> temperature;
        std::optional<std::string> grinderRatio;
        std::optional<uint8_t> waterAmount;
        std::optional<uint8_t> milkFoamAmount;
    };

    Product(std::string&& name, std::string&& code, std::optional<ItemsOption>&& strength, std::optional<ItemsOption>&& temperature, std::optional<MinMaxOption>&& waterAmount, std::optional<MinMaxOption> milkFoamAmount, std::optional<ItemsOption>&& grinderRatio) : name(std::move(name)),
                                                                                                                                                                                                                              code(std::move(code)),
                                                                                                                                                                                                                              strength(std::move(strength)),
                                                                                                                                                                                                                              temperature(std::move(temperature)),
                                                                                                                                                                                                                              waterAmount(std::move(waterAmount)),
                                                                                                                                                                                                                              milkFoamAmount(std::move(milkFoamAmount)),
                                                                                                                                                                                                                              grinderRatio(std::move(grinderRatio)) {}

    [[nodiscard]] std::string to_bt_command(const BrewOptions& options = {}) const;
    [[nodiscard]] size_t code_to_size_t() const;
} __attribute__((aligned(128)));

struct Alert {
    size_t bit;
    std::string name;
    std::string type;

    Alert(size_t bit, std::string&& name, std::string&& type) : bit(bit),
                                                                name(std::move(name)),
                                                                type(std::move(type)) {}
} __attribute__((aligned(128)));

struct MaintenanceCounter {
    std::string name;
    uint16_t count;

    MaintenanceCounter(std::string&& name, uint16_t count) : name(std::move(name)),
                                                             count(count) {}
} __attribute__((aligned(64)));

struct MaintenancePercentage {
    std::string name;
    uint8_t percent;

    MaintenancePercentage(std::string&& name, uint8_t percent) : name(std::move(name)),
                                                                 percent(percent) {}
} __attribute__((aligned(64)));

struct Joe {
    std::string dated;
    const Machine* machine;
    std::vector<Product> products;
    std::vector<Alert> alerts;
    std::vector<MaintenanceCounter> maintenanceCounters;
    std::vector<MaintenancePercentage> maintenancePercentages;

    size_t statTotalCount{0};

    // Events:
    eventpp::CallbackList<void(const std::vector<const Alert*>&)> alertsChangedEventHandler;
    eventpp::CallbackList<void(const std::shared_ptr<Joe>&)> productStatisticCountersChangedEventHandler;
    eventpp::CallbackList<void(const std::vector<MaintenanceCounter>&)> maintenanceCountersChangedEventHandler;
    eventpp::CallbackList<void(const std::vector<MaintenancePercentage>&)> maintenancePercentagesChangedEventHandler;

    Joe(std::string&& dated, const Machine* machine, std::vector<Product>&& products, std::vector<Alert>&& alerts, std::vector<MaintenanceCounter>&& maintenanceCounters, std::vector<MaintenancePercentage>&& maintenancePercentages) : dated(std::move(dated)),
                                                                                                                                                                                                                                         machine(machine),
                                                                                                                                                                                                                                         products(std::move(products)),
                                                                                                                                                                                                                                         alerts(std::move(alerts)),
                                                                                                                                                                                                                                         maintenanceCounters(std::move(maintenanceCounters)),
                                                                                                                                                                                                                                         maintenancePercentages(std::move(maintenancePercentages)) {}
} __attribute__((aligned(128)));

std::unordered_map<size_t, const Machine> load_machines(const std::filesystem::path& path);
std::shared_ptr<Joe> load_joe(const Machine* machine);
//---------------------------------------------------------------------------
}  // namespace jutta_bt_proto
//---------------------------------------------------------------------------