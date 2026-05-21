#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void nivona_brew_init(void);
bool nivona_brew_start(int16_t process_value, bool two_cups);
void nivona_brew_cancel(void);
bool nivona_brew_active(void);

#ifdef __cplusplus
}
#endif
