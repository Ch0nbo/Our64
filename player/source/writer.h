/* Our64 player: one thread on core 2 for every SD write, so the game never
 * waits for the card. A job is a function run on that thread. Jobs in lower
 * slots run first: a resume point always reaches the SD before the in-game
 * saves captured with it, so a power cut can never leave a resume point
 * older than a save. Save states and their backups (WRITER_STATE) go after
 * those, and the player log's periodic copy (WRITER_LOG) last.
 * Posting a slot that is still waiting replaces it. */
#ifndef OUR64_WRITER_H
#define OUR64_WRITER_H

#ifdef __cplusplus
extern "C" {
#endif

enum { WRITER_RESUME, WRITER_EEP, WRITER_SRA, WRITER_FLA, WRITER_MPK, WRITER_STATE, WRITER_LOG, WRITER_SLOTS };

int  writer_start(void);              /* 0 on success */
void writer_post(int slot, void (*job)(void));
int  writer_pending(int slot);        /* waiting or running */
void writer_drain(void);              /* waits until every job has run */
void writer_stop(void);               /* drain, then end the thread */
void writer_lock(void);               /* guards data a job shares with the CPU core */
void writer_unlock(void);
/* A job's write that took over 2 s goes in the log, the public one too: a
 * player's report then shows a slow or nearly full card. */
void writer_slow(const char *what, double ms);
int writer_job_slot(void);           /* the job running now, -1 when idle (no lock: the watchdog) */
double writer_job_ms(void);          /* how long it has run, 0 when idle */

#ifdef __cplusplus
}
#endif
#endif
