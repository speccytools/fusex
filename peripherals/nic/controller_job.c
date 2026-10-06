#include "config.h"
#include "controller_job.h"
#include "../fs/http_downloader.h"
#include <pthread.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct controller_job {
    pthread_t thread;
    bool created, running, completed, stopping;
    void (*execute)(void *);
    void *request;
} jobs[2];

static void *worker(void *argument)
{
    struct controller_job *job = argument;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!job->running && !job->stopping) pthread_cond_wait(&changed, &lock);
        if (job->stopping) break;
        pthread_mutex_unlock(&lock);
        job->execute(job->request);
        pthread_mutex_lock(&lock);
        job->running = false;
        job->completed = true;
        pthread_cond_broadcast(&changed);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

int controller_job_start(unsigned channel, void (*execute)(void *), void *request)
{
    if (channel >= 2) return -1;
    pthread_mutex_lock(&lock);
    struct controller_job *job = &jobs[channel];
    if (job->running || job->completed) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    if (!job->created) {
        if (pthread_create(&job->thread, NULL, worker, job)) {
            pthread_mutex_unlock(&lock);
            return -1;
        }
        job->created = true;
    }
    job->execute = execute;
    job->request = request;
    job->running = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

bool controller_job_is_running(unsigned channel)
{
    pthread_mutex_lock(&lock);
    bool running = jobs[channel].running;
    pthread_mutex_unlock(&lock);
    return running;
}

bool controller_job_poll(unsigned channel)
{
    pthread_mutex_lock(&lock);
    bool completed = jobs[channel].completed;
    jobs[channel].completed = false;
    pthread_mutex_unlock(&lock);
    return completed;
}

void controller_job_cancel_and_wait(void)
{
    pthread_mutex_lock(&lock);
    while (jobs[0].running || jobs[1].running) {
        pthread_mutex_unlock(&lock);
        http_downloader_cancel_all();
        pthread_mutex_lock(&lock);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 10000000;
        if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        if (jobs[0].running || jobs[1].running)
            pthread_cond_timedwait(&changed, &lock, &until);
    }
    pthread_mutex_unlock(&lock);
}

void controller_job_shutdown(void)
{
    controller_job_cancel_and_wait();
    pthread_mutex_lock(&lock);
    for (unsigned channel = 0; channel < 2; ++channel) jobs[channel].stopping = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    for (unsigned channel = 0; channel < 2; ++channel) {
        if (jobs[channel].created) pthread_join(jobs[channel].thread, NULL);
        jobs[channel] = (struct controller_job){0};
    }
}
