#ifndef UPDATE_H
#define UPDATE_H

#include "common.h"

// Check for updates
BOOL update_check(HWND hwnd_parent);

// Download and install update
BOOL update_download_and_install(HWND hwnd_parent);

// Get update URL
void get_update_url(wchar_t *url, size_t url_size);

#endif // UPDATE_H
