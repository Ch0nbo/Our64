/* Our64 player: the updater on the console (Settings > Update; update.h has
 * the portable parts). One thread, low priority on core 2 like Fetch covers,
 * does one job at a time over HTTPS (curl and mbedTLS, GitHub's certificates
 * checked against third_party/cacert):
 *   a check     GitHub's newest release of Our64: its version and files
 *   a download  the app and GLideN64.custom.ini into <root>/update/, each
 *               checked against the size and SHA-256 GitHub names; and, when
 *               asked, shaders.zip, whose packs and keys are merged into
 *               <root>/shaders/games/
 * The menu thread starts a job, follows it each frame and ends it
 * (updater_finish joins the thread). Then updater_install puts the new app
 * in place of the running one: Aroma's loader lets go of the file, it is
 * renamed away, the download renamed in, and
 * player_relaunch starts it as Our64 closes. */
#ifndef OUR64_UPDATE_WIIU_H
#define OUR64_UPDATE_WIIU_H
#include "update.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
	UPDATE_OK,
	UPDATE_STOPPED,     /* B, or Our64 closing */
	UPDATE_NO_NETWORK,  /* OUR-0411: not connected, or no internet (the name lookup failed) */
	UPDATE_NO_SERVER,   /* OUR-0412: GitHub not reached, or its answer was not a release */
	UPDATE_CERTIFICATE, /* OUR-0413: GitHub's certificate refused: the console's date is wrong, most often */
	UPDATE_DAMAGED,     /* OUR-0414: a download's size or SHA-256 is not what GitHub named */
	UPDATE_CARD,        /* OUR-0206: a file could not be written to the SD card */
	UPDATE_NO_START,    /* OUR-0415: no memory or no thread */
	UPDATE_NOT_PLACED,  /* OUR-0416: the new app could not replace the running one */
};

/* Once, as Our64 starts: where it runs from, and what an earlier update left
 * behind removed (the old app, half downloads). */
void updater_boot(const char *root);
/* 0: started. A check's answer is updater_release once it finished with UPDATE_OK. */
int  updater_check_start(void);
/* app: our64.wuhb and the ini; shaders: shaders.zip, merged into the card's. */
int  updater_download_start(const update_release *r, int app, int shaders);
int  updater_job(void);               /* 0 none, 1 a check, 2 a download: started and not finished with updater_finish */
int  updater_running(void);           /* the job's thread still works */
void updater_progress(int *step, unsigned *done, unsigned *total); /* a download: USTEP_ (setup.h) */
void updater_stop(void);              /* ask it to stop; updater_finish still ends it */
int  updater_finish(void);            /* joins the thread: UPDATE_ */
const update_release *updater_release(void); /* the last good check's; version "" before one */
int  updater_shaders_new(void);       /* that release's shaders are not the ones on the card */
const char *updater_notes(void);      /* that release's notes as lines (update_release_notes), "" none */
/* After a download with the app: UPDATE_OK (then leave through
 * player_relaunch: Our64 closes and the new one starts), or UPDATE_NOT_PLACED
 * with the running app still in place. */
int  updater_install(void);
/* main.c, as Our64 closes after updater_install: the new app instead of the
 * Wii U Menu. 0 when Aroma's loader took it. */
int  updater_launch(void);
/* What a result reads as under a page's header, with its code; "" for UPDATE_OK. */
void updater_summary(int result, char *out, unsigned size);

#ifdef __cplusplus
}
#endif
#endif
