/* Our64 player: box art from libretro-thumbnails over plain HTTP, the
 * portable parts: the cover's file name and URL path, the request and the
 * reply (covers.c). Plain C. Every output is NUL-terminated inside size; one
 * that does not fit comes out "" (a cut name or path would ask for another
 * file). */
#ifndef OUR64_COVERS_H
#define OUR64_COVERS_H
#ifdef __cplusplus
extern "C" {
#endif

#define COVERS_HOST "thumbnails.libretro.com"

/* libretro-thumbnails' file name for a title: each of & * / : ` < > ? \ | " becomes _
 * (RetroArch's gfx_thumbnail.c). */
void covers_thumb_name(const char *title, char *out, unsigned size);
/* "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/<percent-encoded name>.png" */
void covers_path(const char *thumb_name, char *out, unsigned size);
/* The request: GET <path> HTTP/1.0, Host: thumbnails.libretro.com,
 * User-Agent: Our64/<version>, Connection: close. Its length, or -1 when it
 * does not fit. */
int covers_request(const char *path, char *out, unsigned size);
/* A whole reply of n bytes: 200 with a body -> 200, the body's offset and
 * length (Content-Length when given); else the status (404...), or -1 when
 * it is not HTTP, its headers never end or the body is shorter than its
 * Content-Length (a cut-off download). */
int covers_reply(const unsigned char *reply, unsigned n, unsigned *body, unsigned *body_n);
/* The second try: the ROM's file name without its folder and extension
 * ("roms/Banjo-Kazooie (USA).z64" -> "Banjo-Kazooie (USA)"). */
void covers_file_title(const char *file, char *out, unsigned size);

#ifdef __cplusplus
}
#endif
#endif
