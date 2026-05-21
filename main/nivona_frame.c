#include "nivona_frame.h"
#include "nivona_crypto.h"
#include "nivona_gatt.h"
#include "nivona_dispatch.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "nivona_frame";

// ---- Parser state ------------------------------------------------------

static uint8_t s_buf[MAX_FRAME_BYTES];
static size_t  s_len = 0;
static bool    s_in_frame = false;

// ---- Handshake state ---------------------------------------------------

static bool    s_handshake_done = false;
static uint8_t s_key_prefix[KEY_PREFIX_LEN] = {0};

bool nivona_frame_handshake_complete(void) { return s_handshake_done; }
const uint8_t *nivona_frame_key_prefix(void) { return s_key_prefix; }

void nivona_frame_mark_handshake(const uint8_t kp[2]) {
    memcpy(s_key_prefix, kp, KEY_PREFIX_LEN);
    s_handshake_done = true;
}

// ---- Helpers -----------------------------------------------------------

static uint8_t checksum(const uint8_t *data, size_t len) {
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) s = (uint8_t)(s + data[i]);
    return (uint8_t)(~s);
}

// Map command string → (length, is_encrypted)
static bool cmd_info(const char *buf, size_t avail,
                     int *out_len, bool *out_encrypted) {
    if (avail < 1) return false;
    // Unencrypted single-char ACK/NAK — only valid BEFORE the handshake
    // completes. After we're encrypted, byte 0x41 ('A') / 0x4E ('N') is
    // a perfectly normal first ciphertext byte and must not be
    // interpreted as a stale ACK. (Audit-V3 finding I-Block1.)
    if (!s_handshake_done && (buf[0] == 'A' || buf[0] == 'N')) {
        *out_len = 1;
        *out_encrypted = false;
        return true;
    }
    if (avail >= 2 && buf[0] == 'H') {
        *out_len = 2;
        *out_encrypted = true;
        return true;
    }
    return false;
}

// ---- Frame parsing ----------------------------------------------------

// Parser counters
uint32_t g_diag_try_parse_called = 0;
uint32_t g_diag_cs_mismatch = 0;
uint32_t g_diag_unknown_cmd = 0;
uint8_t  g_diag_last_decrypt[32] = {0};
size_t   g_diag_last_decrypt_len = 0;
uint8_t  g_diag_last_recv_cs = 0;
uint8_t  g_diag_last_expect_cs = 0;

static void try_parse(void) {
    // Non-reentrant: plain[] and cs_in[] are static so they survive
    // across calls — that's a deliberate choice (NimBLE host stack
    // is too tight for 512-byte locals). The trade-off is that
    // try_parse MUST NOT be called from inside itself or from any
    // handler that re-feeds bytes into the parser. Catch that here
    // explicitly so a future regression doesn't silently overwrite
    // a parent invocation's buffers. (Audit-V3 finding I-Block1.)
    static bool s_in_parse = false;
    if (s_in_parse) {
        ESP_LOGE(TAG, "try_parse called recursively — would clobber "
                      "the static plain[]/cs_in[] buffers. Ignoring "
                      "the inner call.");
        return;
    }
    s_in_parse = true;
    g_diag_try_parse_called++;
    ESP_LOGD(TAG, "try_parse len=%u first=%02x last=%02x",
             (unsigned)s_len, s_len > 0 ? s_buf[0] : 0,
             s_len > 0 ? s_buf[s_len-1] : 0);
    if (s_len < 4) {
        ESP_LOGW(TAG, "too short");
        goto out;
    }
    const uint8_t *p = s_buf;
    if (p[0] != FRAME_START || p[s_len - 1] != FRAME_END) {
        ESP_LOGW(TAG, "S/E mismatch first=%02x last=%02x", p[0], p[s_len-1]);
        goto out;
    }

    int cmd_len = 0;
    bool encrypted = false;
    if (!cmd_info((const char *)(p + 1), s_len - 2, &cmd_len, &encrypted)) {
        g_diag_unknown_cmd++;
        ESP_LOGW(TAG, "unknown cmd prefix: %02x %02x",
                 s_len >= 3 ? p[1] : 0, s_len >= 4 ? p[2] : 0);
        goto out;
    }

    char cmd[3] = {0};
    memcpy(cmd, p + 1, cmd_len);

    // data_part = everything between cmd and trailing E
    size_t data_start = 1 + cmd_len;
    size_t data_end = s_len - 1;  // exclude E
    if (data_end <= data_start) {
        // No payload / no checksum — malformed
        ESP_LOGW(TAG, "frame too short for cmd=%s", cmd);
        goto out;
    }
    size_t data_len = data_end - data_start;

    static uint8_t plain[MAX_FRAME_BYTES];  // static: avoid NimBLE task stack overflow
    if (encrypted) {
        nivona_rc4_with_master_key(p + data_start, plain, data_len);
    } else {
        memcpy(plain, p + data_start, data_len);
    }

    // Last byte of plain = checksum
    uint8_t recv_cs = plain[data_len - 1];
    size_t payload_len = data_len - 1;

    // Capture for diagnostics — but redact the 2-byte session key
    // prefix once the handshake is up. The diag-over-HTTP endpoint
    // is unauthenticated by design (dev-only), and the prefix is the
    // auth token that gates every subsequent frame. Anyone hitting
    // /diag would otherwise harvest a fresh key and forge frames.
    // Pre-handshake frames have no prefix, so we capture them whole.
    {
        size_t skip = (encrypted && s_handshake_done) ? KEY_PREFIX_LEN : 0;
        if (data_len > skip) {
            size_t cap_len = data_len - skip;
            if (cap_len > sizeof(g_diag_last_decrypt)) {
                cap_len = sizeof(g_diag_last_decrypt);
            }
            g_diag_last_decrypt_len = cap_len;
            memcpy(g_diag_last_decrypt, plain + skip, cap_len);
        } else {
            g_diag_last_decrypt_len = 0;
        }
    }

    // Build checksum input: cmd_bytes + payload
    static uint8_t cs_in[MAX_FRAME_BYTES];
    memcpy(cs_in, p + 1, cmd_len);
    memcpy(cs_in + cmd_len, plain, payload_len);
    uint8_t expect_cs = checksum(cs_in, cmd_len + payload_len);
    g_diag_last_recv_cs = recv_cs;
    g_diag_last_expect_cs = expect_cs;
    if (recv_cs != expect_cs) {
        g_diag_cs_mismatch++;
        ESP_LOGW(TAG, "cs mismatch cmd=%s got=%02x want=%02x plain=%02x%02x%02x%02x%02x%02x%02x",
                 cmd, recv_cs, expect_cs,
                 plain[0], plain[1], plain[2], plain[3],
                 payload_len > 4 ? plain[4] : 0,
                 payload_len > 5 ? plain[5] : 0,
                 payload_len > 6 ? plain[6] : 0);
        goto out;
    }

    // If encrypted and handshake done, strip the 2-byte key_prefix at
    // the front of payload (client includes it for authenticated frames).
    const uint8_t *actual_payload = plain;
    size_t actual_len = payload_len;
    if (encrypted && s_handshake_done && payload_len >= KEY_PREFIX_LEN) {
        // Verify that the prefix matches our negotiated one
        if (memcmp(plain, s_key_prefix, KEY_PREFIX_LEN) == 0) {
            actual_payload = plain + KEY_PREFIX_LEN;
            actual_len = payload_len - KEY_PREFIX_LEN;
        }
    }

    extern uint32_t g_diag_frame_parsed;
    g_diag_frame_parsed++;
    ESP_LOGD(TAG, "rx cmd=%s payload_len=%u", cmd, (unsigned)actual_len);
    nivona_dispatch(cmd, actual_payload, actual_len);

out:
    s_in_parse = false;
}

void nivona_frame_reset(void) {
    // Called from nivona_gatt_on_disconnect (NimBLE host task only).
    // Also clear handshake state: a fresh connection must run a fresh
    // HU; otherwise nivona_frame_send would prepend the *previous*
    // peer's session key to the new peer's first response, leaking
    // the old key to whoever connected. In practice the new peer
    // disconnects too because of the bad prefix, but better not to
    // leak in the first place.
    s_len = 0;
    s_in_frame = false;
    s_handshake_done = false;
    memset(s_key_prefix, 0, KEY_PREFIX_LEN);
}

// Expected REQUEST frame sizes (from HA → emulator), indexed by 2nd char
// of cmd "H*". 0 = variable / unknown, use FRAME_END detection.
// Frame = S(1) + cmd(2) + kp(2, post-handshake) + payload + cs(1) + E(1)
static int expected_request_size(char cmd2) {
    switch (cmd2) {
        case 'U': return 11;  // HU: seed(4)+ver(2) — no kp
        case 'V': return 7;   // HV: empty
        case 'X': return 7;   // HX: empty
        case 'I': return 7;   // HI: empty
        case 'R': return 9;   // HR: id(2)
        case 'A': return 9;   // HA: id(2)
        case 'W': return 13;  // HW: id(2)+val(4)
        case 'D': return 9;   // HD: id(2)
        case 'Y': return 11;  // HY: 4 bytes confirm
        case 'Z': return 11;  // HZ: 4 bytes cancel
        case 'E': return 25;  // HE: 18-byte brew payload
        case 'C': return 9;   // HC: id(2)  (n/a for Nivona)
        case 'B': return 0;   // HB: variable — use FRAME_END detection
        case 'J': return 73;  // HJ: 66-byte recipe  (n/a for Nivona)
        default:  return 0;
    }
}

void nivona_frame_feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (!s_in_frame) {
            if (b == FRAME_START) {
                s_in_frame = true;
                s_len = 0;
                s_buf[s_len++] = b;
            }
            continue;
        }
        if (s_len >= MAX_FRAME_BYTES) {
            ESP_LOGW(TAG, "frame overflow, resetting");
            s_in_frame = false;
            s_len = 0;
            continue;
        }
        s_buf[s_len++] = b;

        // Determine expected size once we have cmd bytes [1..2]
        int expected = 0;
        if (s_len >= 3 && s_buf[1] == 'H') {
            expected = expected_request_size((char)s_buf[2]);
        }

        if (expected > 0) {
            // Fixed-size command: terminate ONLY when we've collected that many
            // bytes AND last byte is FRAME_END. Ignore spurious 0x45 inside
            // encrypted payload.
            if (s_len == (size_t)expected) {
                try_parse();
                s_in_frame = false;
                s_len = 0;
            }
        } else {
            // Fallback: terminate on first 0x45 (variable-length or single-char cmds)
            if (b == FRAME_END) {
                try_parse();
                s_in_frame = false;
                s_len = 0;
            }
        }
    }
}

// ---- Frame building / sending -----------------------------------------

void nivona_frame_send(const char *cmd,
                       const uint8_t *payload, size_t payload_len,
                       bool include_key_prefix,
                       bool encrypt) {
    // STACK-allocated, not static. nivona_frame_send is called from
    //   - the NimBLE host task (ACK/NACK/response inside dispatch)
    //   - brew_task / cycle_task (push_status every 500 ms)
    //   - the CLI / telnet task (cmd_send et al.)
    // A static buffer here used to be torn whenever two of those
    // overlapped — protocol-corrupt frames on the wire. MAX_FRAME_BYTES
    // is 512 and every caller task has ≥ 4 kB of stack, so the buffer
    // fits comfortably with no synchronisation cost.
    uint8_t frame[MAX_FRAME_BYTES];
    size_t pos = 0;
    size_t cmd_len = cmd ? strlen(cmd) : 0;

    // Bounds check BEFORE any write into frame[]. Compute the total
    // bytes we'll write:
    //   FRAME_START (1) + cmd (cmd_len) +
    //   optional key prefix (KEY_PREFIX_LEN) +
    //   payload (payload_len) +
    //   checksum (1) + FRAME_END (1).
    // The 2-byte cmd_len assumption (max "HX" / "HA" / "HU" etc.) is
    // also enforced — a typo'd 3-byte opcode would silently slide past
    // checks downstream.
    size_t kp_len = (include_key_prefix && s_handshake_done) ? KEY_PREFIX_LEN : 0;
    size_t required = 1 + cmd_len + kp_len + payload_len + 1 + 1;
    if (cmd_len < 1 || cmd_len > 2) {
        ESP_LOGE(TAG, "frame_send: cmd length %u outside [1,2] (cmd=%.4s)",
                 (unsigned)cmd_len, cmd ? cmd : "(null)");
        return;
    }
    if (required > sizeof(frame)) {
        ESP_LOGE(TAG, "frame_send: required %u > frame %u (cmd=%s payload_len=%u)",
                 (unsigned)required, (unsigned)sizeof(frame),
                 cmd, (unsigned)payload_len);
        return;
    }

    frame[pos++] = FRAME_START;
    memcpy(frame + pos, cmd, cmd_len);
    pos += cmd_len;

    size_t enc_start = pos;  // where encryption begins

    if (include_key_prefix && s_handshake_done) {
        memcpy(frame + pos, s_key_prefix, KEY_PREFIX_LEN);
        pos += KEY_PREFIX_LEN;
    }
    if (payload && payload_len) {
        memcpy(frame + pos, payload, payload_len);
        pos += payload_len;
    }

    // Checksum over cmd + (kp) + payload
    uint8_t cs = checksum(frame + 1, pos - 1);
    frame[pos++] = cs;

    if (encrypt) {
        nivona_rc4_with_master_key(frame + enc_start, frame + enc_start,
                                    pos - enc_start);
    }

    frame[pos++] = FRAME_END;

    ESP_LOGI(TAG, "tx cmd=%s frame_len=%u", cmd, (unsigned)pos);

    // Chunk into MTU-sized notifications
    size_t off = 0;
    while (off < pos) {
        size_t n = pos - off;
        if (n > BLE_MTU) n = BLE_MTU;
        nivona_gatt_notify(frame + off, n);
        off += n;
    }
}
