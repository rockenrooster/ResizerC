#ifndef WINDOW_H
#define WINDOW_H

#include "common.h"

// Create main window
HWND window_create(HINSTANCE hinstance, AppState *state);

// Window procedure
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// Initialize UI controls
BOOL window_init_controls(HWND hwnd, AppState *state);

// Update statistics display
void window_update_stats(AppState *state);

// Enable/disable controls during processing
void window_set_processing_state(AppState *state, BOOL processing);

#endif // WINDOW_H
