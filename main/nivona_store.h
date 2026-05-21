#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIVONA_ALPHA_MAX 64

void nivona_store_init(void);

// Numerical registers (HR/HW): int16 id → int32 value
int32_t nivona_store_get_num(int16_t id);
void    nivona_store_set_num(int16_t id, int32_t value);
bool    nivona_store_has_num(int16_t id);

// Erase a single numerical register — used by HD (SetDefaultNumericValue).
// Next read returns 0 (or whatever seed_defaults re-seeds on reboot).
void    nivona_store_erase_num(int16_t id);

// Batched numerical writes — one NVS open + one commit at the end,
// instead of one per `set_num`. Use this on hot paths that mutate many
// counters at once (brew completion writes 6-9 wear/counter values,
// each previously costing one flash erase+write).
//
// Contract: nivona_store_batch_t is currently a single shared slot;
// only one batch can be active at a time. brew_task and cycle_task
// already serialise on their own mutexes, so this is fine in practice.
// Calling set_num on a batched id concurrently is safe (the memory
// store is updated immediately); only the NVS-side write is deferred.
typedef struct nivona_store_batch nivona_store_batch_t;
nivona_store_batch_t *nivona_store_batch_begin(void);
void                  nivona_store_batch_set_num(nivona_store_batch_t *b,
                                                 int16_t id, int32_t value);
void                  nivona_store_batch_end(nivona_store_batch_t *b);

// Alphanumeric registers (HA/HB): int16 id → up to 64 bytes UTF-8
// Returns number of bytes written into out (0..NIVONA_ALPHA_MAX).
size_t  nivona_store_get_alpha(int16_t id, uint8_t *out, size_t max);
void    nivona_store_set_alpha(int16_t id, const uint8_t *data, size_t len);

// Dump all known registers to log (for CLI debugging).
void    nivona_store_dump(void);

#ifdef __cplusplus
}
#endif
