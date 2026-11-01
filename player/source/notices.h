/* Our64 player: controller notices, the one thing drawn over a running game
 * (Zach, 2026-10-16): a player joined, a controller left or came back, a
 * battery is low. Plain C, so tests/ runs it on the PC.
 *
 * Notices show at the top, oldest first, at most NOTICES_SHOWN at a time;
 * the rest wait their turn, and a notice's time starts when it shows. A
 * notice with the same key as one already there replaces it, so "Player 1's
 * controller disconnected" becomes "Player 1: Pro Controller is back". */
#ifndef OUR64_NOTICES_H
#define OUR64_NOTICES_H
#ifdef __cplusplus
extern "C" {
#endif

#define NOTICE_TEXT 112
#define NOTICES_MAX 8
#define NOTICES_SHOWN 2

enum { NOTICE_INFO, NOTICE_WARN };

typedef struct {
	int key, kind;
	char line1[NOTICE_TEXT], line2[NOTICE_TEXT]; /* line2 may be empty */
	double secs;     /* how long it shows; < 0 until removed */
	double shown_us; /* when it first showed; < 0 not yet */
} notice;

typedef struct {
	notice n[NOTICES_MAX];
	int count;
} notices;

void notices_init(notices *q);
/* A full queue drops its oldest timed notice to make room. */
void notices_add(notices *q, int key, int kind, const char *line1, const char *line2, double secs);
void notices_remove(notices *q, int key);
/* The notices to draw now (up to NOTICES_SHOWN, oldest first); expired ones
 * go, newly shown ones start their time. */
int notices_visible(notices *q, double now_us, const notice **out);

#ifdef __cplusplus
}
#endif
#endif
