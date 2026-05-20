#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Own address type resolved during the on_sync callback (typically
// BLE_OWN_ADDR_RANDOM after we set the random static address from the
// hardware MAC). Read-only after init — exposed via a getter rather
// than an extern global so a stray write from elsewhere cannot break
// pairing. (Audit-V3 finding I-encapsulation.)
uint8_t nivona_ble_own_addr_type(void);
void nivona_ble_own_addr_set(uint8_t t);

void nivona_ble_start_advertising(void);
const char *nivona_ble_device_name(void);

#ifdef __cplusplus
}
#endif
