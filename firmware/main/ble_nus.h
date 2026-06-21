#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Callback for inbound text from the phone (NUS RX characteristic).
// Currently unused; chunk 2 of the BLE work will wire up a command parser
// against this signature.
typedef void (*ble_nus_rx_cb_t)(const char *data, size_t len);

// Initialise NimBLE, register the Nordic UART Service, and start advertising
// as `device_name`. Pass NULL for rx_cb if you don't yet need inbound writes.
esp_err_t ble_nus_init(const char *device_name, ble_nus_rx_cb_t rx_cb);

// Send a string as a notification on the NUS TX characteristic.
// No-op if no client is connected. Thread-safe.
void ble_nus_send(const char *s);

#ifdef __cplusplus
}
#endif
