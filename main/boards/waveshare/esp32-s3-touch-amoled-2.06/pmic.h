#pragma once

#include "axp2101.h"
#include <esp_log.h>

class Pmic : public Axp2101 {
private:
    esp_err_t initialization_error_ = ESP_OK;

    void InitializeRegister(uint8_t reg, uint8_t value) {
        if (initialization_error_ != ESP_OK) return;
        const uint8_t bytes[] = {reg, value};
        initialization_error_ = i2c_master_transmit(i2c_device_, bytes, sizeof(bytes), 100);
    }

public:
    ~Pmic() {
        // A failed candidate must release its handle before the bus is reset.
        const auto err = i2c_master_bus_rm_device(i2c_device_);
        if (err != ESP_OK) ESP_LOGW("WavesharePmic", "PMIC handle release failed: %s", esp_err_to_name(err));
    }

    esp_err_t InitializationError() const { return initialization_error_; }

    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        InitializeRegister(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        InitializeRegister(0x27, 0x10);  // hold 4s to power off

        // Disable All DCs but DC1
        InitializeRegister(0x80, 0x01);
        // Keep the required analog rails on throughout initialization. The
        // PMIC survives an ESP32 reset, so an off/on sequence can leave them
        // off if initialization is interrupted. A powered-down shared-bus
        // peripheral may also prevent the later enable write from reaching it.
        InitializeRegister(0x90, 0x03);
        InitializeRegister(0x91, 0x00);

        // Set DC1 to 3.3V
        InitializeRegister(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        InitializeRegister(0x92, (3300 - 500) / 100);
        InitializeRegister(0x93, (3300 - 500) / 100);

        InitializeRegister(0x64, 0x02); // CV charger voltage setting to 4.1V

        InitializeRegister(0x61, 0x02); // set Main battery precharge current to 50mA
        InitializeRegister(0x62, 0x0A); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        InitializeRegister(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};
