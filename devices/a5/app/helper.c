/*
 * cascadia-helper: the part of the Cascadia app that needs root, installed
 * setuid.  Takes one word and uses nothing from the caller's environment:
 *
 *   status    checks, as one line of JSON
 *   boot      the same checks, then sync and kloader; iOS stops there
 *   update    the newest release's kernel into /kernelcache, this machine's
 *             layer kept; the one before stays as /kernelcache.prev
 *   rollback  /kernelcache and /kernelcache.prev change places
 *
 * Only files named in a root-owned manifest are trusted, each by its SHA-1.
 * kloader and iBEC come from the computer once; this package carries neither.
 * kloader needs a real uid of 0: as mobile it cannot find the kernel base.
 *
 * An update reads the network only through cascadia-fetch, run as mobile; a
 * downloaded file counts only when its size and SHA-256 are the ones in the
 * release's manifest (prebuilt/<channel>/a5.json in the repository).
 */
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pcless.h"

#define DIR_		"/var/root/cascadia"
#define MANIFEST	DIR_ "/manifest"
#define KLOADER		DIR_ "/kloader"
#define IBEC		DIR_ "/ibec"
#define RELEASE		DIR_ "/release"	/* the tag these boot files came from */
#define RELEASE_PREV	DIR_ "/release.prev"
#define CHANNEL		DIR_ "/channel"	/* stable (absent) or experimental */
#define UPD		DIR_ "/update"
#define KC		"/kernelcache"
#define KC_NEW		"/kernelcache.new"
#define KC_PREV		"/kernelcache.prev"
#define FETCH		"/usr/libexec/cascadia-fetch"
#define MANIFEST_URL	"https://raw.githubusercontent.com/rzxvx/Project-Cascadia/main/prebuilt/%s/a5.json"
#define MOBILE		501
#define VOLUME		"/dev/rdisk0s1s3"
#define MODEL		"iPad2,5"
#define BUILD		"12H321"
#define MAX_FILES	8
#define MIN_VOLUME_MIB	1024

#define GET_BLOCK_SIZE	_IOR('d', 24, uint32_t)
#define GET_BLOCK_COUNT	_IOR('d', 25, uint64_t)

struct file {
	char path[128];
	char want[41];
	char have[41];
	int ok;
};

static struct file files[MAX_FILES];
static int nfiles;

static int sha1_file(const char *path, char out[41])
{
	static unsigned char buf[1 << 20];
	unsigned char md[CC_SHA1_DIGEST_LENGTH];
	CC_SHA1_CTX c;
	ssize_t n;
	int fd = open(path, O_RDONLY);
	int i;

	if (fd < 0)
		return -1;
	CC_SHA1_Init(&c);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		CC_SHA1_Update(&c, buf, (CC_LONG)n);
	close(fd);
	if (n < 0)
		return -1;
	CC_SHA1_Final(md, &c);
	for (i = 0; i < CC_SHA1_DIGEST_LENGTH; i++)
		snprintf(out + i * 2, 3, "%02x", md[i]);

	return 0;
}

/* Root's, and nobody else may write it: everything else hangs off this. */
static int load_manifest(void)
{
	struct stat st;
	char line[256];
	FILE *f = fopen(MANIFEST, "r");

	if (!f)
		return -1;
	if (fstat(fileno(f), &st) || st.st_uid != 0 || (st.st_mode & 022)) {
		fclose(f);
		return -1;
	}
	while (nfiles < MAX_FILES && fgets(line, sizeof(line), f)) {
		struct file *e = &files[nfiles];

		if (sscanf(line, "%40s %127s", e->want, e->path) != 2 ||
		    e->path[0] != '/')
			continue;
		nfiles++;
	}
	fclose(f);

	return nfiles ? 0 : -1;
}

static int has_file(const char *path)
{
	int i;

	for (i = 0; i < nfiles; i++)
		if (!strcmp(files[i].path, path))
			return 1;

	return 0;
}

static int check_files(void)
{
	int i, bad = 0;

	for (i = 0; i < nfiles; i++) {
		struct file *e = &files[i];

		e->have[0] = 0;
		e->ok = !sha1_file(e->path, e->have) &&
			!strcmp(e->have, e->want);
		bad += !e->ok;
	}

	return bad;
}

static long volume_mib(void)
{
	uint32_t size = 0;
	uint64_t count = 0;
	int fd = open(VOLUME, O_RDONLY);

	if (fd < 0)
		return 0;
	if (ioctl(fd, GET_BLOCK_SIZE, &size) ||
	    ioctl(fd, GET_BLOCK_COUNT, &count))
		count = 0;
	close(fd);

	return (long)(count * size >> 20);
}

static void sysctl_str(const char *name, char *out, size_t len)
{
	out[0] = 0;
	if (sysctlbyname(name, out, &len, NULL, 0))
		out[0] = 0;
}

/* Empty when absent or not a plain tag: it goes into JSON as it is. */
static void read_tag(const char *path, char *out, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t i;

	out[0] = 0;
	if (!f)
		return;
	if (!fgets(out, (int)len, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
	for (i = 0; out[i]; i++) {
		char c = out[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
			out[0] = 0;
			return;
		}
	}
}

/* One kind word and a line of text, read by the app as it comes. */
static void say(const char *kind, const char *text)
{
	printf("%s %s\n", kind, text);
	fflush(stdout);
}

static int file_ok(const char *path)
{
	int i;

	for (i = 0; i < nfiles; i++)
		if (!strcmp(files[i].path, path))
			return files[i].ok;

	return 0;
}

static const char *want_of(const char *path)
{
	int i;

	for (i = 0; i < nfiles; i++)
		if (!strcmp(files[i].path, path))
			return files[i].want;

	return NULL;
}

/* the manifest again, root's, 0644, replaced in one rename */
static int save_manifest(void)
{
	FILE *f = fopen(MANIFEST ".new", "w");
	int i, fd;

	if (!f)
		return -1;
	for (i = 0; i < nfiles; i++)
		fprintf(f, "%s %s\n", files[i].want, files[i].path);
	fflush(f);
	fd = fileno(f);
	if (fchown(fd, 0, 0) || fchmod(fd, 0644) || fsync(fd)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	return rename(MANIFEST ".new", MANIFEST);
}

static void set_want(const char *path, const char *sha1)
{
	int i;

	for (i = 0; i < nfiles; i++)
		if (!strcmp(files[i].path, path)) {
			snprintf(files[i].want, sizeof(files[i].want), "%s", sha1);
			return;
		}
	if (nfiles < MAX_FILES) {
		snprintf(files[nfiles].path, sizeof(files[nfiles].path), "%s", path);
		snprintf(files[nfiles].want, sizeof(files[nfiles].want), "%s", sha1);
		nfiles++;
	}
}

static int write_tag(const char *path, const char *tag)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	fprintf(f, "%s\n", tag);
	fsync(fileno(f));
	fclose(f);

	return 0;
}

static int read_all(const char *path, unsigned char **buf, size_t *n)
{
	struct stat st;
	int fd = open(path, O_RDONLY);
	size_t got = 0;

	if (fd < 0)
		return -1;
	if (fstat(fd, &st) || !(*buf = malloc((size_t)st.st_size + 1))) {
		close(fd);
		return -1;
	}
	while (got < (size_t)st.st_size) {
		ssize_t r = read(fd, *buf + got, (size_t)st.st_size - got);

		if (r <= 0)
			break;
		got += (size_t)r;
	}
	close(fd);
	if (got != (size_t)st.st_size) {
		free(*buf);
		return -1;
	}
	(*buf)[got] = 0;
	*n = got;

	return 0;
}

static int write_all(const char *path, const unsigned char *d, size_t n)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t put = 0;

	if (fd < 0)
		return -1;
	while (put < n) {
		ssize_t w = write(fd, d + put, n - put);

		if (w <= 0)
			break;
		put += (size_t)w;
	}
	if (put != n || fchmod(fd, 0644) || fsync(fd)) {
		close(fd);
		unlink(path);
		return -1;
	}

	return close(fd);
}

static int sha256_file(const char *path, char out[65])
{
	static unsigned char buf[1 << 20];
	unsigned char md[CC_SHA256_DIGEST_LENGTH];
	CC_SHA256_CTX c;
	ssize_t n;
	int fd = open(path, O_RDONLY), i;

	if (fd < 0)
		return -1;
	CC_SHA256_Init(&c);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		CC_SHA256_Update(&c, buf, (CC_LONG)n);
	close(fd);
	if (n < 0)
		return -1;
	CC_SHA256_Final(md, &c);
	for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
		snprintf(out + i * 2, 3, "%02x", md[i]);

	return 0;
}

/* cascadia-fetch get URL, as mobile; the body into path (root's) */
static int fetch(const char *url, const char *path)
{
	int p[2], status, fd;
	unsigned char buf[65536];
	ssize_t r;
	pid_t pid;

	if (pipe(p))
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		close(p[0]);
		if (dup2(p[1], 1) < 0 || setgid(MOBILE) || setuid(MOBILE) || !getuid())
			_exit(126);
		execl(FETCH, FETCH, "get", url, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	while ((r = read(p[0], buf, sizeof(buf))) > 0)
		if (fd >= 0 && write(fd, buf, (size_t)r) != r) {
			close(fd);
			fd = -1;
		}
	close(p[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) || fd < 0) {
		if (fd >= 0)
			close(fd);
		unlink(path);
		return -1;
	}

	return close(fd);
}

/* "key": "value" (or a number) after from, within the same object */
static int json_str(const char *from, const char *key, char *out, size_t len)
{
	char k[64];
	const char *p, *end = strchr(from, '}'), *e;

	snprintf(k, sizeof(k), "\"%s\":", key);
	p = strstr(from, k);
	if (!p || (end && p > end))
		return -1;
	p += strlen(k);
	while (*p == ' ')
		p++;
	if (*p == '"')
		p++;
	for (e = p; *e && *e != '"' && *e != ',' && *e != '\n' && *e != '}'; e++)
		;
	if ((size_t)(e - p) >= len)
		return -1;
	memcpy(out, p, (size_t)(e - p));
	out[e - p] = 0;

	return 0;
}

struct rfile {
	const char *name;
	char url[512], sha256[65], size[16];
	char path[128];
};

static int download(const char *json, struct rfile *f, char *msg, size_t ml)
{
	char k[96], have[65];
	const char *at;
	struct stat st;

	snprintf(k, sizeof(k), "\"name\": \"%s\"", f->name);
	at = strstr(json, k);
	if (!at || json_str(at, "url", f->url, sizeof(f->url)) ||
	    json_str(at, "sha256", f->sha256, sizeof(f->sha256)) ||
	    json_str(at, "size", f->size, sizeof(f->size)) ||
	    strncmp(f->url, "https://github.com/rzxvx/Project-Cascadia/releases/download/", 60)) {
		snprintf(msg, ml, "the release has no %s", f->name);
		return -1;
	}
	snprintf(f->path, sizeof(f->path), UPD "/%s", f->name);
	snprintf(msg, ml, "downloading %s (%s bytes)", f->name, f->size);
	say("go", msg);
	if (fetch(f->url, f->path) || stat(f->path, &st) ||
	    (unsigned long long)st.st_size != strtoull(f->size, NULL, 10) ||
	    sha256_file(f->path, have) || strcmp(have, f->sha256)) {
		unlink(f->path);
		snprintf(msg, ml, "%s did not download whole, or does not match its SHA-256", f->name);
		return -1;
	}

	return 0;
}

static int update(const char *installed)
{
	struct rfile ld = { "nand-linux-boot.bin" }, zi = { "nand-zImage-dtb" };
	char channel[32], url[256], version[96], msg[256], newsha[41];
	unsigned char *json = NULL, *kc = NULL, *bundle = NULL, *loader = NULL, *zimage = NULL,
		      *ovl = NULL, *nb = NULL, *img = NULL;
	size_t jn, kn, bn, ln, zn, oo, on, nn, in;
	struct statfs fs;
	int e, rc = 1;

	read_tag(CHANNEL, channel, sizeof(channel));
	if (!channel[0])
		strcpy(channel, "stable");
	mkdir(UPD, 0700);
	snprintf(url, sizeof(url), MANIFEST_URL, channel);
	snprintf(msg, sizeof(msg), "the %s release's manifest", channel);
	say("go", msg);
	if (fetch(url, UPD "/a5.json") || read_all(UPD "/a5.json", &json, &jn) ||
	    json_str((char *)json, "version", version, sizeof(version))) {
		say("no", "could not read the release's manifest (no internet?)");
		goto out;
	}
	if (installed[0] && strcmp(version, installed) <= 0) {
		snprintf(msg, sizeof(msg), "already the newest: %s", installed);
		say("ok", msg);
		rc = 0;
		goto out;
	}
	if (statfs("/", &fs) || (unsigned long long)fs.f_bavail * fs.f_bsize < (80ull << 20)) {
		say("no", "less than 80 MB free on iOS's System partition");
		goto out;
	}
	if (download((char *)json, &ld, msg, sizeof(msg)) || download((char *)json, &zi, msg, sizeof(msg))) {
		say("no", msg);
		goto out;
	}
	say("go", "taking this iPad's layer out of /kernelcache");
	if (read_all(KC, &kc, &kn) || (e = pcless_unwrap(kc, kn, &bundle, &bn)) ||
	    (e = pcless_overlay(bundle, bn, &oo, &on))) {
		say("no", "/kernelcache cannot be taken apart: install from the computer");
		goto out;
	}
	ovl = malloc(on ? on : 1);
	if (!ovl)
		goto out;
	memcpy(ovl, bundle + oo, on);
	free(bundle);
	bundle = NULL;
	say("go", "building the new /kernelcache");
	if (read_all(ld.path, &loader, &ln) || read_all(zi.path, &zimage, &zn) ||
	    (e = pcless_assemble(loader, ln, zimage, zn, ovl, on, &nb, &nn))) {
		say("no", "the release's kernel does not fit this iPad's layer");
		goto out;
	}
	free(zimage);
	zimage = NULL;
	if ((e = pcless_wrap(kc, kn, nb, nn, &img, &in))) {
		say("no", pcless_error(e));
		goto out;
	}
	free(nb);
	nb = NULL;
	if (write_all(KC_NEW, img, in) || sha1_file(KC_NEW, newsha)) {
		say("no", "could not write /kernelcache.new");
		goto out;
	}
	/* the switch: the old one becomes .prev, then the manifest says so */
	if (rename(KC, KC_PREV) || rename(KC_NEW, KC)) {
		rename(KC_PREV, KC);
		say("no", "could not put the new /kernelcache in place");
		goto out;
	}
	set_want(KC_PREV, want_of(KC));
	set_want(KC, newsha);
	if (save_manifest()) {
		say("no", "could not write the manifest: roll back, or install from the computer");
		goto out;
	}
	write_tag(RELEASE_PREV, installed);
	write_tag(RELEASE, version);
	sync();
	sync();
	snprintf(msg, sizeof(msg), "updated to %s; the one before stays for roll back", version);
	say("ok", msg);
	rc = 0;
out:
	unlink(ld.path);
	unlink(zi.path);
	unlink(UPD "/a5.json");
	free(json);
	free(kc);
	free(bundle);
	free(loader);
	free(zimage);
	free(ovl);
	free(nb);
	free(img);

	return rc;
}

static int rollback(void)
{
	char cur[64], prev[64], a[41], b[41];

	if (!has_file(KC_PREV) || !file_ok(KC_PREV)) {
		say("no", "there is no earlier /kernelcache to go back to");
		return 1;
	}
	read_tag(RELEASE, cur, sizeof(cur));
	read_tag(RELEASE_PREV, prev, sizeof(prev));
	snprintf(a, sizeof(a), "%s", want_of(KC));
	snprintf(b, sizeof(b), "%s", want_of(KC_PREV));
	if (rename(KC, KC_NEW) || rename(KC_PREV, KC) || rename(KC_NEW, KC_PREV)) {
		say("no", "could not swap the files: install from the computer");
		return 1;
	}
	set_want(KC, b);
	set_want(KC_PREV, a);
	if (save_manifest()) {
		say("no", "could not write the manifest: install from the computer");
		return 1;
	}
	write_tag(RELEASE, prev);
	write_tag(RELEASE_PREV, cur);
	sync();
	sync();
	say("ok", prev[0] ? "back to the earlier kernel" : "back to the earlier kernel (not from a release)");

	return 0;
}

int main(int argc, char **argv)
{
	char model[32], build[32], release[64], prev[64], channel[32], msg[256];
	int root, manifest, i, bad;
	long mib;

	if (argc != 2)
		return 2;
	root = !setgid(0) && !setuid(0) && getuid() == 0;
	sysctl_str("hw.machine", model, sizeof(model));
	sysctl_str("kern.osversion", build, sizeof(build));
	manifest = !load_manifest();
	bad = manifest ? check_files() : 0;
	mib = volume_mib();
	read_tag(RELEASE, release, sizeof(release));

	if (!strcmp(argv[1], "status")) {
		printf("{\"model\":\"%s\",\"build\":\"%s\",\"root\":%d,\"manifest\":%d,\"volumeMiB\":%ld,\"release\":\"%s\",\"files\":[",
		       model, build, root, manifest, mib, release);
		for (i = 0; i < nfiles; i++)
			printf("%s{\"path\":\"%s\",\"have\":\"%s\",\"want\":\"%s\",\"ok\":%d}",
			       i ? "," : "", files[i].path, files[i].have,
			       files[i].want, files[i].ok);
		read_tag(RELEASE_PREV, prev, sizeof(prev));
		read_tag(CHANNEL, channel, sizeof(channel));
		printf("],\"rollback\":%d,\"prevRelease\":\"%s\",\"channel\":\"%s\"}\n",
		       has_file(KC_PREV) && file_ok(KC_PREV), prev, channel[0] ? channel : "stable");
		return 0;
	}
	if (!strcmp(argv[1], "update") || !strcmp(argv[1], "rollback")) {
		if (!root || strcmp(model, MODEL) || strcmp(build, BUILD)) {
			say("no", "not root, or not iPad2,5 on 12H321");
			return 1;
		}
		if (!manifest || !has_file(KC) || !file_ok(KC)) {
			say("no", "the boot files do not match the manifest: install them from the computer");
			return 1;
		}
		return !strcmp(argv[1], "update") ? update(release) : rollback();
	}
	if (strcmp(argv[1], "boot"))
		return 2;

	if (!root) {
		say("no", "helper is not running as root");
		return 1;
	}
	if (strcmp(model, MODEL) || strcmp(build, BUILD)) {
		snprintf(msg, sizeof(msg), "this is %s %s, built for %s %s",
			 model, build, MODEL, BUILD);
		say("no", msg);
		return 1;
	}
	if (!manifest || bad || !has_file(KLOADER) || !has_file(IBEC)) {
		say("no", "boot files do not match the manifest");
		return 1;
	}
	if (mib < MIN_VOLUME_MIB) {
		say("no", "no Linux volume");
		return 1;
	}

	say("go", "sync");
	sync();
	say("go", "sync again");
	sync();
	say("go", "kloader: starting iBEC, iOS stops here");
	execl(KLOADER, KLOADER, IBEC, (char *)NULL);
	say("no", "kloader did not start");

	return 1;
}
