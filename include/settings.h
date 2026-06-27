#ifndef SETTINGS_H
#define SETTINGS_H

#include "common.h"

// Settings file path
void get_settings_path(wchar_t *path, size_t path_size);

// Save settings
BOOL settings_save(AppState *state);

// Load settings
BOOL settings_load(AppState *state);

// Apply settings to UI
void settings_apply_to_ui(AppState *state);

// Get settings from UI
void settings_get_from_ui(AppState *state);

// Initialize default settings
void settings_init_defaults(AppState *state);

#endif // SETTINGS_H
