/*
 * cascadia-helper: the part of the Cascadia app that needs root, installed
 * setuid.  Takes one word and uses nothing from the caller's environment:
 *
 *   status   checks, as one line of JSON
 *   boot     the same checks, then sync and kloader; iOS stops there
 *
 * Only files named in a root-owned manifest are trusted, each by its SHA-1.
 * kloader and iBEC come from the computer once; this package carries neither.
 * kloader needs a real uid of 0: as mobile it cannot find the kernel base.
 */
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define DIR_		"/var/root/cascadia"
#define MANIFEST	DIR_ "/manifest"
#define KLOADER		DIR_ "/kloader"
#define IBEC		DIR_ "/ibec"
#define RELEASE		DIR_ "/release"	/* the tag these boot files came from */
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
static void read_release(char *out, size_t len)
{
	FILE *f = fopen(RELEASE, "r");
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

int main(int argc, char **argv)
{
	char model[32], build[32], release[64], msg[256];
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
	read_release(release, sizeof(release));

	if (!strcmp(argv[1], "status")) {
		printf("{\"model\":\"%s\",\"build\":\"%s\",\"root\":%d,\"manifest\":%d,\"volumeMiB\":%ld,\"release\":\"%s\",\"files\":[",
		       model, build, root, manifest, mib, release);
		for (i = 0; i < nfiles; i++)
			printf("%s{\"path\":\"%s\",\"have\":\"%s\",\"want\":\"%s\",\"ok\":%d}",
			       i ? "," : "", files[i].path, files[i].have,
			       files[i].want, files[i].ok);
		printf("]}\n");
		return 0;
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
