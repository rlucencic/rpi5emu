#pragma once

/*
 * Log lines on stderr, as the boards' ESP_LOGx: "I (main) text".
 * EMU_LOGD only with the --verbose option.
 */
#include <stdbool.h>

void emu_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
extern bool emu_log_verbose;

#define EMU_LOGE(tag, ...)  emu_log('E', (tag), __VA_ARGS__)
#define EMU_LOGW(tag, ...)  emu_log('W', (tag), __VA_ARGS__)
#define EMU_LOGI(tag, ...)  emu_log('I', (tag), __VA_ARGS__)
#define EMU_LOGD(tag, ...)  do { if (emu_log_verbose) emu_log('D', (tag), __VA_ARGS__); } while (0)
