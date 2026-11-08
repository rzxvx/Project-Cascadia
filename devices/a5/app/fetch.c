/*
 * cascadia-fetch: the newest published a5-* release on GitHub, as one line of
 * JSON.  iOS 8's TLS can no longer connect to GitHub, so this carries its own
 * (BearSSL) and the root certificates it trusts (trust_anchors.h).
 *
 * Runs as the calling user, never setuid: this is the only code that reads
 * data from the network.  Read-only, and it gives up after 25 seconds.
 */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bearssl.h"
#include "trust_anchors.h"

/* The feed, not the REST API: that allows 60 requests an hour per address. */
#define HOST	"github.com"
#define PATH	"/rzxvx/Project-Cascadia/releases.atom"
#define TAG_KEY	"/releases/tag/"
#define PREFIX	"a5-"
#define MAX_BODY (2u << 20)

static int fail(const char *what, int code)
{
	printf("{\"ok\":0,\"error\":\"%s %d\"}\n", what, code);
	return 1;
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

static int host_connect(void)
{
	struct addrinfo hints, *res, *p;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(HOST, "443", &hints, &res))
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

int main(void)
{
	static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
	static br_ssl_client_context cc;
	static br_x509_minimal_context xc;
	static br_sslio_context io;
	static const char req[] =
		"GET " PATH " HTTP/1.0\r\n"
		"Host: " HOST "\r\n"
		"User-Agent: Cascadia\r\n"
		"Accept: application/atom+xml\r\n"
		"Connection: close\r\n\r\n";
	char latest[64] = "";
	char *buf, *p, *end;
	size_t n = 0;
	int fd, status;

	alarm(25);
	buf = malloc(MAX_BODY + 1);
	if (!buf)
		return fail("memory", 0);
	fd = host_connect();
	if (fd < 0)
		return fail("connect", errno);

	br_ssl_client_init_full(&cc, &xc, TAs, TAs_NUM);
	br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof(iobuf), 1);
	br_ssl_client_reset(&cc, HOST, 0);
	br_sslio_init(&io, &cc.eng, sock_read, &fd, sock_write, &fd);
	if (br_sslio_write_all(&io, req, sizeof(req) - 1) < 0 ||
	    br_sslio_flush(&io) < 0)
		return fail("tls", br_ssl_engine_last_error(&cc.eng));

	while (n < MAX_BODY) {
		int r = br_sslio_read(&io, buf + n, MAX_BODY - n);

		if (r < 0)
			break;
		n += (size_t)r;
	}
	close(fd);
	/* A server that closes without close_notify still gave us what it sent. */
	if (!n)
		return fail("tls", br_ssl_engine_last_error(&cc.eng));
	buf[n] = 0;

	if (sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1)
		return fail("http", 0);
	if (status != 200)
		return fail("http", status);

	p = strstr(buf, "\r\n\r\n");
	if (!p)
		return fail("http", 0);
	while ((p = strstr(p, TAG_KEY))) {
		p += strlen(TAG_KEY);
		end = strchr(p, '"');
		if (!end)
			break;
		if (tag_ok(p, (size_t)(end - p))) {
			char t[64];

			memcpy(t, p, (size_t)(end - p));
			t[end - p] = 0;
			if (strcmp(t, latest) > 0)
				strcpy(latest, t);
		}
		p = end;
	}
	if (!latest[0])
		return fail("no-release", 0);
	printf("{\"ok\":1,\"latest\":\"%s\"}\n", latest);

	return 0;
}
