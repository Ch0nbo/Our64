/* Our64 player: per-game options (Our ROMs, X on a game; 2026-11-11). Kept in
 * config/game-options.txt, one game a line, by ROM file name as favorites are:
 *   <file name><TAB>720p
 * A game without a line draws at 480 lines. 720p is never chosen for the
 * player: 5 of 16 games tested held their speed with it
 * (results/res720-2026-11-11, light-2026-11-11). Plain C: tests/ runs it. */
#ifndef OUR64_GAMEOPT_H
#define OUR64_GAMEOPT_H
#ifdef __cplusplus
extern "C" {
#endif

void gameopt_parse(const char *text);                 /* replaces what is held */
int  gameopt_read(const char *path);                  /* games with options, 0 without a file */
int  gameopt_text(char *out, unsigned size);          /* the file's text; its length (out NULL: only that) */
int  gameopt_lines(const char *file);                 /* 480 or 720 */
int  gameopt_set_lines(const char *file, int lines);  /* 0, or -1 when no room is left */

#ifdef __cplusplus
}
#endif
#endif
