/* Our64 player: controller notices (see notices.h). */
#include "notices.h"

#include <stdio.h>
#include <string.h>

void notices_init(notices *q) { q->count = 0; }

static void drop(notices *q, int i)
{
	memmove(&q->n[i], &q->n[i + 1], (size_t)(q->count - i - 1) * sizeof(notice));
	q->count--;
}

void notices_remove(notices *q, int key)
{
	for (int i = 0; i < q->count; i++)
		if (q->n[i].key == key) {
			drop(q, i);
			return;
		}
}

void notices_add(notices *q, int key, int kind, const char *line1, const char *line2, double secs)
{
	notice *n = NULL;
	for (int i = 0; i < q->count; i++)
		if (q->n[i].key == key)
			n = &q->n[i]; /* replaced where it is, so it keeps its place */
	if (!n) {
		if (q->count == NOTICES_MAX) {
			int i = 0;
			while (i < q->count && q->n[i].secs < 0)
				i++;
			drop(q, i < q->count ? i : 0);
		}
		n = &q->n[q->count++];
	}
	n->key = key;
	n->kind = kind;
	snprintf(n->line1, sizeof(n->line1), "%s", line1 ? line1 : "");
	snprintf(n->line2, sizeof(n->line2), "%s", line2 ? line2 : "");
	n->secs = secs;
	n->shown_us = -1;
}

int notices_visible(notices *q, double now_us, const notice **out)
{
	for (int i = 0; i < q->count;)
		if (q->n[i].secs >= 0 && q->n[i].shown_us >= 0 && now_us >= q->n[i].shown_us + q->n[i].secs * 1e6)
			drop(q, i);
		else
			i++;
	int shown = q->count < NOTICES_SHOWN ? q->count : NOTICES_SHOWN;
	for (int i = 0; i < shown; i++) {
		if (q->n[i].shown_us < 0)
			q->n[i].shown_us = now_us;
		out[i] = &q->n[i];
	}
	return shown;
}
