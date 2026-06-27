// dll_loader.h - Embedded DLL loader for libvips
#ifndef DLL_LOADER_H
#define DLL_LOADER_H

#include "common.h"

// Initialize embedded DLLs (extracts and loads them)
// Pass the HINSTANCE from wWinMain to find embedded resources
BOOL dll_loader_init(HINSTANCE hinstance);

// Cleanup extracted DLLs
void dll_loader_cleanup(void);

// Get the temp directory where DLLs are extracted
const wchar_t* dll_loader_get_temp_dir(void);

#endif // DLL_LOADER_H
