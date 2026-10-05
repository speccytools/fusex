#include "config.h"

#include "engine_job.h"
#include "engine_fs.h"
#include "../../fs/http_downloader.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_changed = PTHREAD_COND_INITIALIZER;
static pthread_t job_thread;
static bool job_joinable, job_running, job_result_pending;
static int job_result;
static struct {
    enginecall_t engine;
    char input_file[130]; /* Includes the mount prefix added at submission. */
    char output_file[128];
    char operation[256];
} job_request;

static void *engine_job_worker(void *unused)
{
    (void)unused;
    char *argv[ENGINE_MAX_ARGS];
    const int argc = engine_argv_parse(job_request.operation, argv, ENGINE_MAX_ARGS);
    const int result = argc < 1 ? -3
        : job_request.engine(job_request.input_file, job_request.output_file, argc, argv);
    pthread_mutex_lock(&job_lock);
    job_result = result;
    job_result_pending = true;
    job_running = false;
    pthread_cond_broadcast(&job_changed);
    pthread_mutex_unlock(&job_lock);
    return NULL;
}

bool engine_job_is_running(void)
{
    pthread_mutex_lock(&job_lock);
    const bool running = job_running;
    pthread_mutex_unlock(&job_lock);
    return running;
}

bool engine_job_poll(int *result)
{
    pthread_mutex_lock(&job_lock);
    const bool completed = job_result_pending && !job_running;
    const int completed_result = job_result;
    if (completed) job_result_pending = false;
    pthread_mutex_unlock(&job_lock);
    if (!completed) return false;
    if (job_joinable) {
        pthread_join(job_thread, NULL);
        job_joinable = false;
    }
    if (result) *result = completed_result;
    return true;
}

int engine_job_start(enginecall_t engine, const char *input_file,
                     const char *output_file, const char *operation)
{
    if (engine_job_is_running()) return -3;
    engine_job_poll(NULL);
    if (!engine || !input_file || !output_file || !operation ||
        strlen(output_file) >= sizeof(job_request.output_file) ||
        strlen(operation) >= sizeof(job_request.operation)) return -3;

    /* Resolve the default mount before leaving the emulation thread. */
    int mount_index;
    const char *source_path;
    if (engine_fs_parse_mount_spec(input_file, &mount_index, &source_path) != 0 ||
        snprintf(job_request.input_file, sizeof(job_request.input_file),
                 "%d:%s", mount_index, source_path) >= (int)sizeof(job_request.input_file))
        return -3;
    job_request.engine = engine;
    strcpy(job_request.output_file, output_file);
    strcpy(job_request.operation, operation);
    pthread_mutex_lock(&job_lock);
    job_running = true;
    pthread_mutex_unlock(&job_lock);
    if (pthread_create(&job_thread, NULL, engine_job_worker, NULL) != 0) {
        pthread_mutex_lock(&job_lock);
        job_running = false;
        pthread_mutex_unlock(&job_lock);
        return -2;
    }
    job_joinable = true;
    return 0;
}

void engine_job_cancel_and_wait(void)
{
    pthread_mutex_lock(&job_lock);
    while (job_running) {
        pthread_mutex_unlock(&job_lock);
        http_downloader_cancel_all();
        pthread_mutex_lock(&job_lock);
        /* Repeat cancellation if the worker was just entering HTTP open. */
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 10000000;
        if (until.tv_nsec >= 1000000000) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }
        if (job_running) pthread_cond_timedwait(&job_changed, &job_lock, &until);
    }
    pthread_mutex_unlock(&job_lock);
    if (job_joinable) {
        pthread_join(job_thread, NULL);
        job_joinable = false;
    }
}
