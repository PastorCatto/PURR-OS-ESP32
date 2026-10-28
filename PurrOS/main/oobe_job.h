/*
 * oobe_job.h - one slow thing at a time, on a worker task, polled by the wizard (purr_oobe.h). The wizard's UI task never
 * waits: it starts a job and asks "done yet?" from a timer. A job reports a message and a progress figure while it runs.
 */
#ifndef OOBE_JOB_H
#define OOBE_JOB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oobe_job oobe_job_t;
typedef int (*oobe_job_fn)(oobe_job_t *job, void *arg);          /* 0 = ok, anything else = failed (set a message first) */

oobe_job_t *oobe_job_new(void);
/* Starts fn(job, arg) on its own task. 0 if started, -1 if this job is still running. `arg` must outlive the job. */
#define OOBE_STACK_SMALL   12288    /* accounts, a password check, a flash erase */
#define OOBE_STACK_NET     16384    /* Wi-Fi scan and connect, a small download over TLS */
#define OOBE_STACK_INSTALL 32768    /* installing an image: manifest, signature check, fetch, flash */
/* Starts the job on its own task with the given stack. If the task cannot be created (no internal RAM) the job fails at once with a
 * message the page shows; the call still returns 0, so the caller just polls. oobe_job_start uses the install size. */
int oobe_job_start_stack(oobe_job_t *job, oobe_job_fn fn, void *arg, unsigned stack_bytes);
int oobe_job_start(oobe_job_t *job, oobe_job_fn fn, void *arg);

/* Called by the main task once the wizard is up, instead of idling: it then runs the install-sized jobs on its own (large) stack.
 * Does not return. Without it, every job gets its own task. */
void oobe_job_host_run(void) __attribute__((noreturn));
/* -1 running, 0 finished ok, 1 failed. msg gets the latest message; *per_mille the latest progress (-1 = none reported). */
int oobe_job_poll(oobe_job_t *job, char *msg, size_t cap, int *per_mille);
int oobe_job_running(const oobe_job_t *job);

/* Called from the worker: the message to show (NULL keeps the old one) and progress in thousandths (-1 keeps it). */
void oobe_job_report(oobe_job_t *job, const char *msg, int per_mille);
/* From the worker, anywhere deep in its call chain: end the job as failed, with this message. Never returns. */
void oobe_job_abort(oobe_job_t *job, const char *msg) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* OOBE_JOB_H */
