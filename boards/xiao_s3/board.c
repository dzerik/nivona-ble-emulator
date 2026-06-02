// boards/xiao_s3/board.c — Seeed XIAO ESP32-S3 profile.
#include "board.h"

static const board_info_t s_info = { .name = "xiao_s3", .has_display = false };

const board_info_t *board_info(void) { return &s_info; }

// XIAO S3 uses an onboard antenna with no software-controlled RF switch,
// so there is nothing to do at early init.
void board_early_init(void) { }
