/* Our64 player: per-game options (gameopt.h). */
#include "gameopt.h"
#include <stdio.h>
#include <string.h>

#define GAMEOPT_MAX 512
static struct { char file[128]; int lines; } g_opt[GAMEOPT_MAX];
static int g_n;

static int find(const char *file)
{
	for (int i = 0; i < g_n; i++)
		if (!strcmp(g_opt[i].file, file))
			return i;
	return -1;
}

void gameopt_parse(const char *text)
{
	g_n = 0;
	while (text && *text) {
		size_t n = strcspn(text, "\n");
		const char *tab = memchr(text, '\t', n);
		if (tab && tab > text && (size_t)(tab - text) < sizeof(g_opt[0].file) && g_n < GAMEOPT_MAX) {
			size_t rest = n - (size_t)(tab + 1 - text);
			while (rest > 0 && (tab[rest] == '\r' || tab[rest] == ' '))
				rest--;
			if (rest == 4 && !memcmp(tab + 1, "720p", 4)) {
				memcpy(g_opt[g_n].file, text, (size_t)(tab - text));
				g_opt[g_n].file[tab - text] = 0;
				if (find(g_opt[g_n].file) < 0) {
					g_opt[g_n].lines = 720;
					g_n++;
				}
			}
		}
		text += n + (text[n] == '\n');
	}
}

int gameopt_read(const char *path)
{
	static char text[GAMEOPT_MAX * 136];
	FILE *f = fopen(path, "rb");
	size_t n = 0;
	if (f) {
		n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
	}
	text[n] = 0;
	gameopt_parse(text);
	return g_n;
}

int gameopt_text(char *out, unsigned size)
{
	int at = 0;
	for (int i = 0; i < g_n; i++) {
		if (g_opt[i].lines != 720)
			continue;
		int n = snprintf(out ? out + at : NULL, out && (unsigned)at < size ? size - (unsigned)at : 0, "%s\t720p\n", g_opt[i].file);
		at += n;
	}
	return at;
}

int gameopt_lines(const char *file)
{
	const int i = file ? find(file) : -1;
	return i >= 0 && g_opt[i].lines == 720 ? 720 : 480;
}

int gameopt_set_lines(const char *file, int lines)
{
	int i = find(file);
	if (i < 0) {
		if (lines != 720)
			return 0;
		if (g_n >= GAMEOPT_MAX || strlen(file) >= sizeof(g_opt[0].file) || strchr(file, '\t') || strchr(file, '\n'))
			return -1;
		i = g_n++;
		snprintf(g_opt[i].file, sizeof(g_opt[i].file), "%s", file);
	}
	g_opt[i].lines = lines == 720 ? 720 : 480;
	return 0;
}
