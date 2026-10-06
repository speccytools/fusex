#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Two persistent workers. Only the emulation thread submits and collects jobs. */
int controller_job_start(unsigned channel, void (*execute)(void *), void *request);
bool controller_job_poll(unsigned channel);
bool controller_job_is_running(unsigned channel);
void controller_job_cancel_and_wait(void);
/* Stop and join the persistent workers before emulator shutdown. */
void controller_job_shutdown(void);
