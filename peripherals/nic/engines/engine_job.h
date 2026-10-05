#pragma once

#include <stdbool.h>
#include "engine.h"

/* One background engine job. Submit, poll and cancel from the emulation
 * thread; filesystem work runs on a worker and never touches Z80 registers. */
int engine_job_start(enginecall_t engine, const char *input_file,
                     const char *output_file, const char *operation);
bool engine_job_is_running(void);
/* Reap a completed job and report its result once. */
bool engine_job_poll(int *result);
/* Abort active network reads and join before mounts or emulator RAM are freed. */
void engine_job_cancel_and_wait(void);
