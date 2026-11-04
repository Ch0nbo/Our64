/* Our64 player: box art from libretro-thumbnails, the portable parts (see
 * covers.h). Plain C. */
#include "covers.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

#define COVERS_DIR "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/"

void covers_thumb_name(const char *title, char *out, unsigned size)
{
	size_t n = strlen(title);
	if (size == 0)
		return;
	if (n >= size) {
		out[0] = 0;
		return;
	}
	for (size_t i = 0; i < n; i++)
		out[i] = strchr("&*/:`<>?\\|\"", title[i]) ? '_' : title[i];
	out[n] = 0;
}

static int unreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		c == '-' || c == '_' || c == '.' || c == '~';
}

void covers_path(const char *thumb_name, char *out, unsigned size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = sizeof COVERS_DIR - 1;
	if (size == 0)
		return;
	out[0] = 0;
	if (o >= size)
		return;
	memcpy(out, COVERS_DIR, o);
	for (const unsigned char *p = (const unsigned char *)thumb_name; *p; p++) {
		if (unreserved(*p)) {
			if (o + 1 >= size)
				goto cut;
			out[o++] = (char)*p;
		} else {
			if (o + 3 >= size)
				goto cut;
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 15];
		}
	}
	if (o + 4 >= size)
		goto cut;
	memcpy(out + o, ".png", 5);
	return;
cut:
	out[0] = 0;
}

int covers_request(const char *path, char *out, unsigned size)
{
	int n;
	if (size == 0)
		return -1;
	n = snprintf(out, size,
		"GET %s HTTP/1.0\r\n"
		"Host: " COVERS_HOST "\r\n"
		"User-Agent: Our64/" OUR64_VERSION "\r\n"
		"Connection: close\r\n"
		"\r\n", path);
	if (n < 0 || (unsigned)n >= size) {
		out[0] = 0;
		return -1;
	}
	return n;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* The header line [p, end) is "<name>:<value>": the value's start, or NULL. */
static const unsigned char *header_value(const unsigned char *p, const unsigned char *end, const char *name)
{
	size_t k = strlen(name);
	if ((size_t)(end - p) <= k || p[k] != ':')
		return NULL;
	for (size_t i = 0; i < k; i++)
		if (lower(p[i]) != lower((unsigned char)name[i]))
			return NULL;
	p += k + 1;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	return p;
}

int covers_reply(const unsigned char *reply, unsigned n, unsigned *body, unsigned *body_n)
{
	const unsigned char *end = reply + n, *p, *head_end = NULL;
	int status = 0;
	unsigned long long length = 0;
	int has_length = 0;
	/* "HTTP/1.x SSS ..." */
	if (n < 12 || memcmp(reply, "HTTP/", 5))
		return -1;
	for (p = reply + 5; p < end && *p != ' '; p++)
		if (*p == '\r' || *p == '\n')
			return -1;
	if (end - p < 4)
		return -1;
	for (int i = 1; i <= 3; i++) {
		if (p[i] < '0' || p[i] > '9')
			return -1;
		status = status * 10 + (p[i] - '0');
	}
	if (p + 4 < end && p[4] != ' ' && p[4] != '\r')
		return -1;
	/* the headers end at the first blank line */
	for (const unsigned char *q = p; q + 4 <= end; q++)
		if (q[0] == '\r' && q[1] == '\n' && q[2] == '\r' && q[3] == '\n') {
			head_end = q + 2; /* the blank line's own CRLF */
			break;
		}
	if (!head_end || status < 100 || status > 599)
		return -1;
	if (status != 200)
		return status;
	/* each header line after the status line */
	for (p = (const unsigned char *)memchr(reply, '\n', n) + 1; p < head_end;) {
		const unsigned char *eol = (const unsigned char *)memchr(p, '\r', (size_t)(head_end - p));
		const unsigned char *v;
		if (!eol)
			break;
		v = header_value(p, eol, "Content-Length");
		if (v) {
			if (v == eol)
				return -1;
			for (length = 0; v < eol; v++) {
				if (*v == ' ' || *v == '\t')
					break;
				if (*v < '0' || *v > '9' || length > n)
					return -1;
				length = length * 10 + (unsigned)(*v - '0');
			}
			has_length = 1;
		}
		p = eol + 2;
	}
	*body = (unsigned)(head_end + 2 - reply);
	*body_n = n - *body;
	if (has_length) {
		if (length > *body_n)
			return -1; /* cut off */
		*body_n = (unsigned)length;
	}
	return 200;
}

void covers_file_title(const char *file, char *out, unsigned size)
{
	const char *base = strrchr(file, '/'), *dot;
	size_t n;
	base = base ? base + 1 : file;
	dot = strrchr(base, '.');
	n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
	if (size == 0)
		return;
	if (n >= size) {
		out[0] = 0;
		return;
	}
	memcpy(out, base, n);
	out[n] = 0;
}
