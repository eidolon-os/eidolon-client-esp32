#ifndef EIDOLON_I2C_BUS_RECOVERY_H_
#define EIDOLON_I2C_BUS_RECOVERY_H_

#include <driver/gpio.h>

// Free a slave that is holding SDA low, by clocking SCL until it lets go.
//
// A slave reset in the middle of a transfer keeps driving SDA while it waits
// for the clocks that would finish the byte it was sending. The bus then reads
// as busy to every master that comes after, and `i2c_master_transmit` answers
// ESP_ERR_INVALID_STATE for a device that is present and working.
//
// It matters most for the PMIC: because the PMIC powers this board, its own
// state survives a power cycle at the USB connector, so a bus it is holding
// stays held across what a person would reasonably call "turning it off and on
// again". Recovering the bus is the only thing that clears it without opening
// the case.
//
// Nine clocks is the whole procedure — a byte plus its ACK — followed by a STOP
// condition. On an idle bus it changes nothing, so it is always safe to run
// before taking the pins for the I2C peripheral.
//
// Returns true if SDA was released (or was never held).
bool RecoverI2cBus(gpio_num_t sda, gpio_num_t scl);

#endif  // EIDOLON_I2C_BUS_RECOVERY_H_
