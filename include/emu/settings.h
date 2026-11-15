#pragma once

/*
 * Settings, where the boards have NVS: ~/.config/rpi5emu/settings.ini
 * ($XDG_CONFIG_HOME if set), sections as NVS's namespaces ("zx", "zxdisks",
 * "speaker"), shared by every rpi5* machine as NVS is by the board's
 * projects. Each set writes the file (through a temporary one: never half
 * written). Safe from any thread.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* False (and *value / buf untouched) when the key isn't there */
bool emu_setting_get_int(const char *ns, const char *key, int32_t *value);
bool emu_setting_get_str(const char *ns, const char *key, char *buf, size_t len);

void emu_setting_set_int(const char *ns, const char *key, int32_t value);
void emu_setting_set_str(const char *ns, const char *key, const char *value);
void emu_setting_erase(const char *ns, const char *key);

/* The file's path */
const char *emu_settings_path(void);
