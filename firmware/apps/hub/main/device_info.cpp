/*
 * Basic Information for the hub: vendor and product names, serial number and
 * version strings. Controllers (Apple Home included) show these and name the
 * accessory from ProductName; the SDK's example provider says TEST_VENDOR /
 * TEST_PRODUCT.
 *
 * Vendor and product IDs stay the Matter test IDs (0xFFF1 / 0x8000) until
 * the device has a real VID and certification.
 */
#include "device_info.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <esp_mac.h>
#include <esp_matter_providers.h>
#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>
#include <platform/CHIPDeviceError.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <sdkconfig.h>

namespace {

using chip::MutableByteSpan;
using chip::DeviceLayer::DeviceInstanceInfoProvider;

/* From the CHIP Kconfig (Component config > CHIP Device Layer > Device
 * Identification Options); test IDs until there is a real VID. */
constexpr uint16_t VENDOR_ID = CONFIG_DEVICE_VENDOR_ID;
constexpr uint16_t PRODUCT_ID = CONFIG_DEVICE_PRODUCT_ID;
constexpr uint16_t HARDWARE_VERSION = CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION;

constexpr size_t MAC_LEN = 6;
/* "LN-" + 12 hex digits + NUL. */
constexpr size_t SERIAL_LEN = 16;

CHIP_ERROR copy_string(const char *value, char *buf, size_t buf_size)
{
    const size_t len = strlen(value);
    if (len == 0) {
        return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    }
    if (len >= buf_size) {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }
    memcpy(buf, value, len + 1);
    return CHIP_NO_ERROR;
}

class hub_device_info : public DeviceInstanceInfoProvider {
public:
    CHIP_ERROR GetVendorName(char *buf, size_t buf_size) override
    {
        return copy_string(CONFIG_HUB_VENDOR_NAME, buf, buf_size);
    }
    CHIP_ERROR GetVendorId(uint16_t &vendor_id) override
    {
        vendor_id = VENDOR_ID;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetProductName(char *buf, size_t buf_size) override
    {
        return copy_string(CONFIG_HUB_PRODUCT_NAME, buf, buf_size);
    }
    CHIP_ERROR GetProductId(uint16_t &product_id) override
    {
        product_id = PRODUCT_ID;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetPartNumber(char *buf, size_t buf_size) override { return copy_string("LN-HUB-C6", buf, buf_size); }
    CHIP_ERROR GetProductURL(char *buf, size_t buf_size) override
    {
        return copy_string(CONFIG_HUB_PRODUCT_URL, buf, buf_size);
    }
    CHIP_ERROR GetProductLabel(char *buf, size_t buf_size) override
    {
        return copy_string("Lochness hub for the lock unit", buf, buf_size);
    }
    /* "LN-" + the Wi-Fi MAC: stable, unique, and not a secret. */
    CHIP_ERROR GetSerialNumber(char *buf, size_t buf_size) override
    {
        /* esp_read_mac(ESP_MAC_WIFI_STA) always writes 6 bytes; the efuse
         * default MAC is 8 bytes (EUI-64) on 802.15.4 chips like the C6. */
        std::array<uint8_t, MAC_LEN> mac{};
        if (esp_read_mac(mac.data(), ESP_MAC_WIFI_STA) != ESP_OK) {
            return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
        }
        std::array<char, SERIAL_LEN> serial{};
        const int len = snprintf(serial.data(), serial.size(), "LN-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
                                 mac[3], mac.at(4), mac.at(5)); // NOLINT(readability-magic-numbers): MAC bytes
        if (len < 0 || static_cast<size_t>(len) >= serial.size()) {
            return CHIP_ERROR_INTERNAL;
        }
        return copy_string(serial.data(), buf, buf_size);
    }
    CHIP_ERROR GetManufacturingDate(uint16_t & /*year*/, uint8_t & /*month*/, uint8_t & /*day*/) override
    {
        return CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND;
    }
    CHIP_ERROR GetHardwareVersion(uint16_t &hardware_version) override
    {
        hardware_version = HARDWARE_VERSION;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR GetHardwareVersionString(char *buf, size_t buf_size) override
    {
        return copy_string("ESP32-C6 DevKit", buf, buf_size);
    }
    CHIP_ERROR GetRotatingDeviceIdUniqueId(MutableByteSpan & /*unique_id_span*/) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
};

} // namespace

void device_info_init()
{
    /* esp-matter keeps the pointer, so the provider lives for the program. */
    static hub_device_info provider;
    esp_matter::set_custom_device_instance_info_provider(&provider);
}
