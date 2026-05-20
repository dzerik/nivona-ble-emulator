#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIVONA_RC4_KEY_LEN 32

// Symmetric RC4 stream cipher (in-place safe, i.e. out may equal in).
// Generic primitive — the caller supplies the key. Callers that want
// the bundled master key should use nivona_rc4_with_master_key().
void nivona_rc4(const uint8_t *key, size_t key_len,
                const uint8_t *in, uint8_t *out, size_t len);

// Convenience wrapper that applies the firmware's bundled RC4 master
// key. Internal — the key bytes themselves are file-static inside
// nivona_crypto.c and never exported as a symbol. Keeps the key out
// of the .rodata symbol table dump that any holder of firmware.elf
// could grep. (Audit-V3 finding M-Block1.)
void nivona_rc4_with_master_key(const uint8_t *in, uint8_t *out, size_t len);

extern const uint8_t NIVONA_HU_TABLE[256];

// 2-round Nivona HU verifier over buf[start..start+count), writes 2 bytes
// into out[0..1].
void nivona_hu_verifier(const uint8_t *buf, size_t start, size_t count,
                        uint8_t out[2]);

#ifdef __cplusplus
}
#endif
