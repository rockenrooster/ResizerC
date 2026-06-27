#ifndef PROCESSING_H
#define PROCESSING_H

#include "common.h"

// Start processing
void processing_start(AppState *state);

// Stop processing
void processing_stop(AppState *state);

// Check if processing is complete
BOOL processing_is_complete(AppState *state);

// Processing thread function
DWORD WINAPI processing_thread(LPVOID param);

#endif // PROCESSING_H
