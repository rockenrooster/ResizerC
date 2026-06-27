#ifndef DRAGDROP_H
#define DRAGDROP_H

#include "common.h"

// Initialize drag-drop for a window
BOOL dragdrop_init(HWND hwnd);

// Handle WM_DROPFILES message (now uses background scanning)
void dragdrop_handle_drop(AppState *state, HDROP hdrop);

// Add files to the list (direct, for non-drop usage)
void dragdrop_add_file(AppState *state, const wchar_t *path);

// Add folder recursively (direct, for non-drop usage)
void dragdrop_add_folder(AppState *state, const wchar_t *path);

// Cancel ongoing scan operation
void dragdrop_cancel_scan(AppState *state);

// Initialize hash table for duplicate detection
void dragdrop_init_hash_table(AppState *state, int estimated_files);

// Clear hash table
void dragdrop_clear_hash_table(AppState *state);

// Populate ListView with all files (called after scan completes)
void dragdrop_populate_listview(AppState *state);

#endif // DRAGDROP_H
