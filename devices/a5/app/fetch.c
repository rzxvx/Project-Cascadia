/*
 * cascadia-fetch: GitHub over HTTPS for the Cascadia app.  iOS 8's TLS can no
 * longer connect to GitHub, so this carries its own (BearSSL) and the root
 * certificates it trusts (trust_anchors.h).
 *
 *   cascadia-fetch            the newest published a5-* release, one line of JSON
 *   cascadia-fetch get URL    URL's body on stdout (redirects followed), exit 0
 *                             only for a whole 200 response
 *
 * Runs as the calling user, never setuid: this is the only code that reads
 * data from the network.  It writes nowhere but stdout; the helper, which
 * runs it as mobile for an update, decides what to keep (by SHA-256).
 * Only GitHub's own hosts, and a deadline (25 s for the check, 10 min a file).
 */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bearssl.h"
#include "trust_anchors.h"

/* The feed, not the REST API: that allows 60 requests an hour per address. */
#define FEED	"https://github.com/rzxvx/Project-Cascadia/releases.atom"
#define TAG_KEY	"/releases/tag/"
#define PREFIX	"a5-"
#define MAX_FEED (2u << 20)
#define MAX_FILE (64u << 20)
#define MAX_HEAD 16384

static const char *const hosts[] = {
	"github.com",
	"raw.githubusercontent.com",
	"release-assets.githubusercontent.com",
	"objects.githubusercontent.com",
};

/* stdout for the release check; stderr when stdout carries a file */
static FILE *errout;

static int fail(const char *what, int code)
{
	fprintf(errout ? errout : stdout, "{\"ok\":0,\"error\":\"%s %d\"}\n", what, code);
	return 1;
}

/* n bytes more of the body: to out, or into *mem (max bytes at most) */
static int sink(FILE *out, char **mem, size_t max, size_t *n, const void *d, size_t len)
{
	if (!len)
		return 0;
	if (*n + len > max)
		return fail("too-big", 0);
	if (out) {
		if (fwrite(d, 1, len, out) != len)
			return fail("write", errno);
	} else {
		memcpy(*mem + *n, d, len);
	}
	*n += len;
	return 0;
}

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
	for (;;) {
		ssize_t r = read(*(int *)ctx, buf, len);

		if (r > 0)
			return (int)r;
		if (r < 0 && errno == EINTR)
			continue;
		return -1;
	}
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
	for (;;) {
		ssize_t r = write(*(int *)ctx, buf, len);

		if (r > 0)
			return (int)r;
		if (r < 0 && errno == EINTR)
			continue;
		return -1;
	}
}

static int host_connect(const char *host)
{
	struct addrinfo hints, *res, *p;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, "443", &hints, &res))
		return -1;
	for (p = res; p; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, p->ai_addr, p->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	return fd;
}

/* https://HOST/PATH, HOST one of ours */
static int split_url(const char *url, char *host, size_t hl, char *path, size_t pl)
{
	const char *h = url + 8, *s;
	size_t i, n;

	if (strncmp(url, "https://", 8))
		return -1;
	s = strchr(h, '/');
	n = s ? (size_t)(s - h) : strlen(h);
	if (!n || n >= hl || strlen(s ? s : "/") >= pl)
		return -1;
	memcpy(host, h, n);
	host[n] = 0;
	snprintf(path, pl, "%s", s ? s : "/");
	for (i = 0; path[i]; i++)
		if ((unsigned char)path[i] <= ' ')
			return -1;
	for (i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
		if (!strcmp(host, hosts[i]))
			return 0;

	return -1;
}

/*
 * GET url; the body goes to out (or into *mem, up to max) -- redirects followed
 * up to four times.  0 for a whole 200 response.
 */
static int get(const char *url, FILE *out, char **mem, size_t max, size_t *got, int depth)
{
	static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
	static br_ssl_client_context cc;
	static br_x509_minimal_context xc;
	static br_sslio_context io;
	char host[128], path[2048], req[2400], *body, *p;
	static char head[MAX_HEAD + 1];
	unsigned char buf[16384];
	size_t hn = 0, n = 0, clen = (size_t)-1;
	int fd, status, r;

	if (depth > 4 || split_url(url, host, sizeof(host), path, sizeof(path)))
		return fail("url", depth);
	fd = host_connect(host);
	if (fd < 0)
		return fail("connect", errno);
	br_ssl_client_init_full(&cc, &xc, TAs, TAs_NUM);
	br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof(iobuf), 1);
	br_ssl_client_reset(&cc, host, 0);
	br_sslio_init(&io, &cc.eng, sock_read, &fd, sock_write, &fd);
	snprintf(req, sizeof(req),
		 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Cascadia\r\nConnection: close\r\n\r\n",
		 path, host);
	if (br_sslio_write_all(&io, req, strlen(req)) < 0 || br_sslio_flush(&io) < 0) {
		close(fd);
		return fail("tls", br_ssl_engine_last_error(&cc.eng));
	}

	/* the header, then whatever of the body came with it */
	for (;;) {
		if (hn == MAX_HEAD) {
			close(fd);
			return fail("http-head", 0);
		}
		r = br_sslio_read(&io, head + hn, MAX_HEAD - hn);
		if (r < 0) {
			close(fd);
			return fail("tls", br_ssl_engine_last_error(&cc.eng));
		}
		hn += (size_t)r;
		head[hn] = 0;
		if ((body = strstr(head, "\r\n\r\n")))
			break;
	}
	body += 4;
	if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1) {
		close(fd);
		return fail("http", 0);
	}
	if (status >= 301 && status <= 308 && status != 304) {
		char loc[2048];

		for (p = head; (p = strchr(p, '\n')) && p < body; p++)
			if (!strncasecmp(p + 1, "Location:", 9))
				break;
		close(fd);
		if (!p || sscanf(p + 10, " %2047s", loc) != 1)
			return fail("redirect", status);
		return get(loc, out, mem, max, got, depth + 1);
	}
	if (status != 200) {
		close(fd);
		return fail("http", status);
	}
	for (p = head; (p = strchr(p, '\n')) && p < body; p++)
		if (!strncasecmp(p + 1, "Content-Length:", 15))
			clen = strtoul(p + 16, NULL, 10);

	/* a server that closes without close_notify still gave us what it sent */
	{
		size_t first = hn - (size_t)(body - head);

		if (sink(out, mem, max, &n, body, first)) {
			close(fd);
			return 1;
		}
		while ((r = br_sslio_read(&io, buf, sizeof(buf))) >= 0)
			if (sink(out, mem, max, &n, buf, (size_t)r)) {
				close(fd);
				return 1;
			}
	}
	close(fd);
	if (clen != (size_t)-1 && n != clen)
		return fail("short", (int)n);
	*got = n;

	return 0;
}

static int tag_ok(const char *t, size_t n)
{
	size_t i;

	if (n < strlen(PREFIX) || n > 60 || strncmp(t, PREFIX, strlen(PREFIX)))
		return 0;
	for (i = 0; i < n; i++) {
		char c = t[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
			return 0;
	}

	return 1;
}

static int latest(void)
{
	char best[64] = "", *buf, *p, *end;
	size_t n = 0;

	alarm(25);
	buf = malloc(MAX_FEED + 1);
	if (!buf)
		return fail("memory", 0);
	if (get(FEED, NULL, &buf, MAX_FEED, &n, 0))
		return 1;
	buf[n] = 0;
	for (p = buf; (p = strstr(p, TAG_KEY)); p = end) {
		p += strlen(TAG_KEY);
		end = strchr(p, '"');
		if (!end)
			break;
		if (tag_ok(p, (size_t)(end - p))) {
			char t[64];

			memcpy(t, p, (size_t)(end - p));
			t[end - p] = 0;
			if (strcmp(t, best) > 0)
				strcpy(best, t);
		}
	}
	if (!best[0])
		return fail("no-release", 0);
	printf("{\"ok\":1,\"latest\":\"%s\"}\n", best);

	return 0;
}

int main(int argc, char **argv)
{
	size_t n = 0;

	if (argc == 1)
		return latest();
	if (argc != 3 || strcmp(argv[1], "get"))
		return 2;
	alarm(600);
	/* the body is stdout's; an error goes to stderr instead */
	errout = stderr;
	if (get(argv[2], stdout, NULL, MAX_FILE, &n, 0)) {
		fflush(stdout);
		return 1;
	}
	fflush(stdout);
	fprintf(stderr, "{\"ok\":1,\"bytes\":%zu}\n", n);

	return 0;
}
