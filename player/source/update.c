/* Our64 player: the updater's portable parts (see update.h). */
#include "update.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- versions ---- */

int update_version_cmp(const char *a, const char *b)
{
	if (*a == 'v' || *a == 'V')
		a++;
	if (*b == 'v' || *b == 'V')
		b++;
	for (;;) {
		char *ea, *eb;
		const unsigned long na = strtoul(a, &ea, 10), nb = strtoul(b, &eb, 10);
		if (na != nb)
			return na < nb ? -1 : 1;
		const int more_a = ea != a && *ea == '.', more_b = eb != b && *eb == '.';
		if (!more_a && !more_b)
			return 0;
		a = more_a ? ea + 1 : "0";
		b = more_b ? eb + 1 : "0";
	}
}

/* ---- the release's JSON: just enough to walk objects and arrays ---- */

static const char *ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

/* p at a string's opening quote: past its closing one, or NULL. */
static const char *skip_string(const char *p)
{
	if (*p != '"')
		return NULL;
	for (p++; *p && *p != '"'; p++)
		if (*p == '\\' && p[1])
			p++;
	return *p == '"' ? p + 1 : NULL;
}

/* Past the value at p (after white space), or NULL when the text is not JSON. */
static const char *skip_value(const char *p, int depth)
{
	p = ws(p);
	if (depth > 32)
		return NULL;
	if (*p == '"')
		return skip_string(p);
	if (*p == '{' || *p == '[') {
		const char close = *p == '{' ? '}' : ']';
		const int object = *p == '{';
		p = ws(p + 1);
		if (*p == close)
			return p + 1;
		for (;;) {
			if (object) {
				p = skip_string(ws(p));
				if (!p || *(p = ws(p)) != ':')
					return NULL;
				p++;
			}
			p = skip_value(p, depth + 1);
			if (!p)
				return NULL;
			p = ws(p);
			if (*p == close)
				return p + 1;
			if (*p != ',')
				return NULL;
			p++;
		}
	}
	const char *start = p;
	while (*p && !strchr(",}] \t\r\n", *p))
		p++;
	return p > start ? p : NULL;
}

/* The value of key in the object at p ("{"), or NULL. */
static const char *member(const char *p, const char *key)
{
	p = ws(p);
	if (*p != '{')
		return NULL;
	p = ws(p + 1);
	const size_t n = strlen(key);
	while (*p == '"') {
		const char *end = skip_string(p);
		if (!end)
			return NULL;
		const int found = (size_t)(end - p) == n + 2 && !memcmp(p + 1, key, n);
		p = ws(end);
		if (*p != ':')
			return NULL;
		p = ws(p + 1);
		if (found)
			return p;
		p = skip_value(p, 0);
		if (!p)
			return NULL;
		p = ws(p);
		if (*p != ',')
			return NULL;
		p = ws(p + 1);
	}
	return NULL;
}

/* The string at p into out, its escapes read (\uXXXX kept only for ASCII). 0 or -1. */
static int string_value(const char *p, char *out, unsigned size)
{
	if (!p || *p != '"')
		return -1;
	unsigned n = 0;
	for (p++; *p && *p != '"'; p++) {
		char c = *p;
		if (c == '\\') {
			c = *++p;
			if (!c)
				return -1;
			if (c == 'n') c = '\n';
			else if (c == 't') c = '\t';
			else if (c == 'r') c = '\r';
			else if (c == 'u') {
				char hex[5] = "";
				for (int i = 0; i < 4 && isxdigit((unsigned char)p[1]); i++)
					hex[i] = *++p;
				const unsigned long u = strtoul(hex, NULL, 16);
				c = u < 128 ? (char)u : '?';
			}
		}
		if (n + 1 >= size)
			return -1; /* too long for a name, a tag or a link of ours */
		out[n++] = c;
	}
	out[n] = 0;
	return *p == '"' ? 0 : -1;
}

static void asset_read(const char *obj, update_asset *a)
{
	char digest[96] = "";
	memset(a, 0, sizeof(*a));
	if (string_value(member(obj, "browser_download_url"), a->url, sizeof(a->url)))
		a->url[0] = 0;
	const char *size = member(obj, "size");
	if (size)
		a->size = (unsigned)strtoul(size, NULL, 10);
	if (!string_value(member(obj, "digest"), digest, sizeof(digest)) && !strncmp(digest, "sha256:", 7) &&
	    strlen(digest + 7) == 64) {
		for (int i = 0; i < 64; i++)
			a->sha256[i] = (char)tolower((unsigned char)digest[7 + i]);
		a->sha256[64] = 0;
	}
}

typedef struct {
	const char *obj;  /* the release's object in the JSON */
	char version[24]; /* its tag without the "v" */
} release_ref;

static int flag_true(const char *obj, const char *key)
{
	const char *v = member(obj, key);
	return v && !strncmp(v, "true", 4);
}

/* The release at obj, when it has a version tag and is neither a draft nor a
 * pre-release. */
static int release_ref_read(const char *obj, release_ref *r)
{
	char tag[64];
	if (string_value(member(obj, "tag_name"), tag, sizeof(tag)))
		return 0;
	const char *v = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
	if (!isdigit((unsigned char)*v) || flag_true(obj, "draft") || flag_true(obj, "prerelease"))
		return 0;
	r->obj = obj;
	snprintf(r->version, sizeof(r->version), "%.*s", (int)sizeof(r->version) - 1, v);
	return 1;
}

/* The releases in json (GitHub's list, or one release) at version from and
 * up (from NULL: all), highest version first, at most max of them. */
static int releases_listed(const char *json, release_ref *out, int max, const char *from)
{
	int n = 0;
	const char *p = ws(json);
	const int list = *p == '[';
	if (list)
		p = ws(p + 1);
	while (*p == '{') {
		release_ref r;
		if (release_ref_read(p, &r) && (!from || update_version_cmp(r.version, from) >= 0)) {
			int at = n;
			while (at > 0 && update_version_cmp(out[at - 1].version, r.version) < 0)
				at--;
			if (at < max) {
				const int keep = n < max ? n : max - 1;
				memmove(out + at + 1, out + at, (size_t)(keep - at) * sizeof(*out));
				out[at] = r;
				if (n < max)
					n++;
			}
		}
		if (!list)
			break;
		p = skip_value(p, 0);
		if (!p)
			break;
		p = ws(p);
		if (*p != ',')
			break;
		p = ws(p + 1);
	}
	return n;
}

int update_parse_release(const char *json, update_release *out)
{
	release_ref newest;
	memset(out, 0, sizeof(*out));
	if (!skip_value(json, 0) || releases_listed(json, &newest, 1, NULL) != 1)
		return -1;
	snprintf(out->version, sizeof(out->version), "%s", newest.version);
	json = newest.obj;
	const char *p = member(json, "assets");
	if (!p || *p != '[')
		return 0;
	p = ws(p + 1);
	while (*p == '{') {
		char name[96];
		if (!string_value(member(p, "name"), name, sizeof(name))) {
			if (!strcmp(name, UPDATE_APP_NAME))
				asset_read(p, &out->app);
			else if (!strcmp(name, UPDATE_INI_NAME))
				asset_read(p, &out->ini);
			else if (!strcmp(name, UPDATE_SHADERS_NAME))
				asset_read(p, &out->shaders);
		}
		p = skip_value(p, 0);
		if (!p)
			break;
		p = ws(p);
		if (*p != ',')
			break;
		p = ws(p + 1);
	}
	return 0;
}

/* ---- the notes ---- */

/* The string at p, its escapes read, as much as fits: for the notes, which
 * may be longer than their room. */
static unsigned string_some(const char *p, char *out, unsigned size)
{
	unsigned n = 0;
	if (!p || *p != '"' || !size)
		return 0;
	for (p++; *p && *p != '"' && n + 1 < size; p++) {
		char c = *p;
		if (c == '\\') {
			c = *++p;
			if (!c)
				break;
			if (c == 'n') c = '\n';
			else if (c == 't') c = ' ';
			else if (c == 'r') continue;
			else if (c == 'u') {
				char hex[5] = "";
				for (int i = 0; i < 4 && isxdigit((unsigned char)p[1]); i++)
					hex[i] = *++p;
				const unsigned long u = strtoul(hex, NULL, 16);
				c = u < 128 ? (char)u : '?';
			}
		}
		if (c != '\r')
			out[n++] = c;
	}
	out[n] = 0;
	return n;
}

typedef struct {
	char *out;
	unsigned size, n, width;
	int full;
	int blank; /* the last line out was empty (or nothing is out yet) */
} notes_out;

static void notes_line(notes_out *o, const char *s, unsigned len)
{
	if (o->full)
		return;
	if (o->n + len + 1 + 4 >= o->size) { /* room kept for "...\n" */
		o->n += (unsigned)sprintf(o->out + o->n, "...\n");
		o->full = 1;
		return;
	}
	memcpy(o->out + o->n, s, len);
	o->n += len;
	o->out[o->n++] = '\n';
}

/* text as lines of at most width bytes, the first after `first`, the rest after `rest`. */
static void notes_wrap(notes_out *o, const char *text, const char *first, const char *rest)
{
	char line[256];
	const unsigned width = o->width < sizeof(line) - 1 ? o->width : sizeof(line) - 1;
	const char *lead = first;
	while (*text == ' ')
		text++;
	do {
		unsigned n = (unsigned)snprintf(line, sizeof(line), "%s", lead);
		size_t take = strlen(text);
		if (n + take > width) {
			take = width - n;
			size_t cut = take;
			while (cut > 0 && text[cut] != ' ')
				cut--;
			if (cut > 0) {
				take = cut;
			} else { /* one long word: cut between characters, never inside one */
				while (take > 1 && ((unsigned char)text[take] & 0xC0) == 0x80)
					take--;
			}
		}
		memcpy(line + n, text, take);
		n += (unsigned)take;
		while (n > 0 && line[n - 1] == ' ')
			n--;
		notes_line(o, line, n);
		text += take;
		while (*text == ' ')
			text++;
		lead = rest;
	} while (*text);
}

/* One Markdown line's marks taken out, in place: ** and `, [words](link), images. */
static void notes_plain(char *s)
{
	char *w = s;
	for (const char *r = s; *r;) {
		if (r[0] == '*' && r[1] == '*') {
			r += 2;
		} else if (*r == '`') {
			r++;
		} else if (*r == '!' && r[1] == '[') {
			const char *end = strchr(r, ')');
			r = end ? end + 1 : r + 1;
		} else if (*r == '[') {
			const char *close = strchr(r, ']');
			if (close && close[1] == '(' && strchr(close, ')')) {
				for (r++; r < close; r++)
					*w++ = *r;
				r = strchr(close, ')') + 1;
			} else {
				*w++ = *r++;
			}
		} else {
			*w++ = *r++;
		}
	}
	*w = 0;
}

static int only_of(const char *s, const char *set)
{
	if (!*s)
		return 0;
	return strspn(s, set) == strlen(s);
}

/* The heading names the page leaves out in every release: installing from
 * the zip, what the zip holds and where to report problems are for the
 * release's page on GitHub, not for an Our64 that updates itself. */
static int notes_left_out(const char *heading)
{
	static const char *const names[] = { "install", "in the zip", "problems" };
	char h[64];
	size_t n = 0;
	for (const char *s = heading; *s && n + 1 < sizeof(h); s++)
		h[n++] = (char)tolower((unsigned char)*s);
	while (n > 0 && h[n - 1] == ' ')
		n--;
	h[n] = 0;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(h, names[i]))
			return 1;
	return 0;
}

/* One line of a release's notes out; heading: a "#" line, or words with a
 * line of dashes or equals under them. *skipping: inside a section left out. */
static void notes_one(notes_out *o, char *text, int heading, int *skipping)
{
	char *q = text + strspn(text, " ");
	if (*q == '#') {
		q += strspn(q, "#");
		q += strspn(q, " ");
		heading = 1;
	}
	if (heading && *q) {
		notes_plain(q);
		*skipping = notes_left_out(q);
	}
	if (*skipping || *q == '>')
		return; /* a quote is a note for GitHub's page (installing from the zip), not for the console */
	if (!*q) {
		if (!o->blank)
			notes_line(o, "", 0);
		o->blank = 1;
	} else if (*q == '|') { /* a table's row; its ruler line is dropped */
		if (!only_of(q, "|-: ")) {
			char row[512] = "";
			for (char *cell = strtok(q, "|"); cell; cell = strtok(NULL, "|")) {
				cell += strspn(cell, " ");
				char *e = cell + strlen(cell);
				while (e > cell && e[-1] == ' ')
					*--e = 0;
				notes_plain(cell);
				const size_t at = strlen(row);
				snprintf(row + at, sizeof(row) - at, "\t%s", cell);
			}
			notes_line(o, row, (unsigned)strlen(row)); /* whole: the page lays its cells out in columns */
			o->blank = 0;
		}
	} else if ((q[0] == '-' || q[0] == '*') && q[1] == ' ') {
		notes_plain(q + 2);
		notes_wrap(o, q + 2, "\xE2\x80\xA2 ", "  ");
		o->blank = 0;
	} else {
		if (!heading)
			notes_plain(q);
		if (heading && !o->blank)
			notes_line(o, "", 0);
		notes_wrap(o, q, "", "");
		o->blank = 0;
	}
}

/* A release's notes (GitHub's Markdown, its escapes already read) as lines. */
static void notes_body(notes_out *o, const char *body)
{
	char prev[512] = "", line[512];
	int have_prev = 0, skipping = 0;
	for (const char *p = body;;) {
		const size_t len = strcspn(p, "\n");
		const int last = !p[len];
		snprintf(line, sizeof(line), "%.*s", (int)(len < sizeof(line) - 1 ? len : sizeof(line) - 1), p);
		char *t = line + strlen(line);
		while (t > line && (t[-1] == ' ' || t[-1] == '\r'))
			*--t = 0;
		const char *s = line + strspn(line, " ");
		/* a line of dashes or equals under words makes those words a heading */
		const int underline = have_prev && prev[0] && (only_of(s, "-") || only_of(s, "=")) && strlen(s) >= 3;
		if (have_prev)
			notes_one(o, prev, underline, &skipping);
		have_prev = !underline;
		if (have_prev)
			snprintf(prev, sizeof(prev), "%s", line);
		if (last) {
			if (have_prev && prev[0])
				notes_one(o, prev, 0, &skipping);
			break;
		}
		p += len + 1;
	}
}

unsigned update_release_notes(const char *json, const char *installed, char *out, unsigned size, unsigned width)
{
	notes_out o = { out, size, 0, width, 0, 1 };
	if (size)
		out[0] = 0;
	if (size < 64 || !skip_value(json, 0))
		return 0;
	release_ref list[UPDATE_NOTES_MAX];
	const int count = releases_listed(json, list, UPDATE_NOTES_MAX, UPDATE_NOTES_FROM);
	char *body = malloc(size);
	if (!body)
		return 0;
	int told = 0;
	for (int i = 0; i < count && !o.full; i++) {
		body[0] = 0;
		string_some(member(list[i].obj, "body"), body, size);
		if (!body[strspn(body, " \r\n\t")])
			continue; /* no notes */
		char head[64];
		if (!told && installed && installed[0] && update_version_cmp(list[i].version, installed) <= 0) {
			told = 1; /* the versions from here on are the one this Our64 is, and older */
			if (!o.blank)
				notes_line(&o, "", 0);
			snprintf(head, sizeof(head), "%cYou have %s", UPDATE_NOTES_YOURS, installed);
			notes_line(&o, head, (unsigned)strlen(head));
		} else if (!o.blank) {
			notes_line(&o, "", 0);
		}
		snprintf(head, sizeof(head), "%cOur64 %s", UPDATE_NOTES_VERSION, list[i].version);
		notes_line(&o, head, (unsigned)strlen(head));
		o.blank = 1; /* its first section's heading follows the version's at once */
		notes_body(&o, body);
	}
	free(body);
	while (o.n > 0 && out[o.n - 1] == '\n' && (o.n < 2 || out[o.n - 2] == '\n'))
		o.n--; /* no empty lines at the end */
	out[o.n] = 0;
	return o.n;
}

/* ---- zip: the central directory, then each file stored or deflated ---- */

static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

int update_zip_open(FILE *f, update_zip *z)
{
	memset(z, 0, sizeof(*z));
	if (fseek(f, 0, SEEK_END))
		return -1;
	const long size = ftell(f);
	if (size < 22)
		return -1;
	/* the end record is the last 22 bytes, or sits before a comment */
	unsigned char tail[22 + 1024];
	const long n = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
	if (fseek(f, size - n, SEEK_SET) || fread(tail, 1, (size_t)n, f) != (size_t)n)
		return -1;
	long at = -1;
	for (long i = n - 22; i >= 0; i--)
		if (le32(tail + i) == 0x06054b50) {
			at = i;
			break;
		}
	if (at < 0)
		return -1;
	z->count = le16(tail + at + 10);
	z->dir_size = le32(tail + at + 12);
	const unsigned dir_at = le32(tail + at + 16);
	if (z->dir_size > 4u * 1024 * 1024 || (long)dir_at + (long)z->dir_size > size)
		return -1;
	z->dir = malloc(z->dir_size ? z->dir_size : 1);
	if (!z->dir || fseek(f, (long)dir_at, SEEK_SET) || fread(z->dir, 1, z->dir_size, f) != z->dir_size) {
		update_zip_close(z);
		return -1;
	}
	return 0;
}

int update_zip_next(update_zip *z, update_zip_entry *e)
{
	if (!z->dir || z->index >= z->count || z->at + 46 > z->dir_size)
		return 0;
	const unsigned char *p = z->dir + z->at;
	if (le32(p) != 0x02014b50)
		return 0;
	const unsigned name = le16(p + 28), extra = le16(p + 30), comment = le16(p + 32);
	if (z->at + 46 + name > z->dir_size)
		return 0;
	e->method = le16(p + 10);
	e->crc = le32(p + 16);
	e->packed = le32(p + 20);
	e->size = le32(p + 24);
	e->offset = le32(p + 42);
	snprintf(e->name, sizeof(e->name), "%.*s", (int)name, (const char *)p + 46);
	if (name >= sizeof(e->name))
		e->name[0] = 0; /* too long to be ours: skipped by its caller */
	z->at += 46 + name + extra + comment;
	z->index++;
	return 1;
}

void *update_zip_read(FILE *f, const update_zip_entry *e)
{
	unsigned char head[30];
	if ((e->method != 0 && e->method != 8) || e->size > 64u * 1024 * 1024 || e->packed > 64u * 1024 * 1024)
		return NULL;
	if (fseek(f, (long)e->offset, SEEK_SET) || fread(head, 1, 30, f) != 30 || le32(head) != 0x04034b50)
		return NULL;
	if (fseek(f, (long)(le16(head + 26) + le16(head + 28)), SEEK_CUR))
		return NULL;
	unsigned char *out = malloc(e->size ? e->size : 1);
	if (!out)
		return NULL;
	int ok = 0;
	if (e->method == 0) {
		ok = e->packed == e->size && fread(out, 1, e->size, f) == e->size;
	} else {
		unsigned char *in = malloc(e->packed ? e->packed : 1);
		if (in && fread(in, 1, e->packed, f) == e->packed) {
			z_stream s;
			memset(&s, 0, sizeof(s));
			if (inflateInit2(&s, -15) == Z_OK) {
				s.next_in = in;
				s.avail_in = e->packed;
				s.next_out = out;
				s.avail_out = e->size;
				ok = inflate(&s, Z_FINISH) == Z_STREAM_END && s.total_out == e->size;
				inflateEnd(&s);
			}
		}
		free(in);
	}
	if (ok && (unsigned)crc32(0, out, e->size) != e->crc)
		ok = 0;
	if (!ok) {
		free(out);
		return NULL;
	}
	return out;
}

void update_zip_close(update_zip *z)
{
	free(z->dir);
	memset(z, 0, sizeof(*z));
}

/* ---- shader packs: "O64K", a version, the number of pairs, then each pair
 * as its length and its .bin file's bytes, whose bytes 8-15 are its key
 * (GLideN64's gx2_ShaderCache.cpp). Words are big-endian. ---- */

static unsigned be32(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(unsigned char *p, unsigned v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

/* The pack's pairs counted, each whole; -1 when it is not a pack. */
static int pack_pairs(const unsigned char *p, unsigned size)
{
	if (!p || size < 12 || memcmp(p, "O64K", 4) || be32(p + 4) != 1)
		return -1;
	const unsigned count = be32(p + 8);
	unsigned pos = 12;
	for (unsigned i = 0; i < count; i++) {
		if (pos + 4 > size)
			return -1;
		const unsigned n = be32(p + pos);
		if (n < 16 || n > size - pos - 4)
			return -1;
		pos += 4 + n;
	}
	return (int)count;
}

/* 1 when the pack's n pairs include key. */
static int pack_has(const unsigned char *pack, int n, const unsigned char *key)
{
	unsigned at = 12;
	for (int k = 0; k < n; k++) {
		if (!memcmp(pack + at + 4 + 8, key, 8))
			return 1;
		at += 4 + be32(pack + at);
	}
	return 0;
}

unsigned char *update_pack_merge(const unsigned char *new_pack, unsigned new_size, const unsigned char *old_pack,
                                 unsigned old_size, unsigned *out_size, unsigned *added)
{
	const int nn = pack_pairs(new_pack, new_size);
	if (nn < 0)
		return NULL;
	int on = pack_pairs(old_pack, old_size);
	if (on < 0)
		on = 0, old_size = 0;
	unsigned char *out = malloc(new_size + old_size);
	if (!out)
		return NULL;
	/* the new pack's pairs end where the last whole one does */
	unsigned end = 12;
	*added = 0;
	for (int i = 0; i < nn; i++) {
		*added += !pack_has(old_pack, on, new_pack + end + 4 + 8);
		end += 4 + be32(new_pack + end);
	}
	memcpy(out, new_pack, end);
	unsigned count = (unsigned)nn, pos = 12;
	for (int i = 0; i < on; i++) {
		const unsigned n = be32(old_pack + pos);
		if (!pack_has(new_pack, nn, old_pack + pos + 4 + 8)) {
			memcpy(out + end, old_pack + pos, 4 + n);
			end += 4 + n;
			count++;
		}
		pos += 4 + n;
	}
	put32(out + 8, count);
	*out_size = end;
	return out;
}

static int key_cmp(const void *a, const void *b)
{
	const unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
	return x < y ? -1 : x > y;
}

/* The hex numbers of text's lines onto keys; the count. */
static unsigned keys_read(const char *text, unsigned long long *keys, unsigned n)
{
	while (text && *text) {
		const size_t len = strcspn(text, "\n");
		if (*text != '#' && len) {
			char *end;
			const unsigned long long k = strtoull(text, &end, 16);
			if (end != text) {
				if (keys)
					keys[n] = k;
				n++;
			}
		}
		text += len + (text[len] == '\n');
	}
	return n;
}

char *update_keys_merge(const char *new_keys, const char *old_keys, unsigned *out_size, unsigned *added)
{
	const unsigned total = keys_read(old_keys, NULL, keys_read(new_keys, NULL, 0));
	unsigned long long *keys = malloc(sizeof(*keys) * (total ? total : 1));
	const size_t comment = new_keys && *new_keys == '#' ? strcspn(new_keys, "\n") : 0;
	char *out = malloc(comment + 1 + (size_t)total * 19 + 1);
	if (!keys || !out) {
		free(keys);
		free(out);
		return NULL;
	}
	/* the card's keys first, counted without repeats: what the merge adds to them */
	const unsigned had = keys_read(old_keys, keys, 0);
	qsort(keys, had, sizeof(*keys), key_cmp);
	unsigned had_once = 0, all_once = 0;
	for (unsigned i = 0; i < had; i++)
		had_once += i == 0 || keys[i] != keys[i - 1];
	keys_read(new_keys, keys, had);
	qsort(keys, total, sizeof(*keys), key_cmp);
	for (unsigned i = 0; i < total; i++)
		all_once += i == 0 || keys[i] != keys[i - 1];
	*added = all_once - had_once;
	unsigned at = 0;
	if (comment) {
		memcpy(out, new_keys, comment);
		at = (unsigned)comment;
		out[at++] = '\n';
	}
	for (unsigned i = 0; i < total; i++)
		if (i == 0 || keys[i] != keys[i - 1])
			at += (unsigned)sprintf(out + at, "0x%016llx\n", keys[i]);
	out[at] = 0;
	free(keys);
	*out_size = at;
	return out;
}
