/* Target-only iosnand control; never run this helper on the build host. */
#define _GNU_SOURCE
#include "iosnand-process.h"
int main(int argc, char **argv)
{
    if (argc != 3) goto usage;
    int ret;
    if (!strcmp(argv[1], "status") || !strcmp(argv[1], "stop"))
        ret = process_control(argv[2], !strcmp(argv[1], "stop"));
    else if (!strcmp(argv[1], "ready")) ret = process_ready_control(argv[2]);
    else if (!strcmp(argv[1], "clean")) ret = process_clean(argv[2]);
    else if (!strcmp(argv[1], "check-directory")) ret = process_check_directory(argv[2]);
    else goto usage;
    if (ret) { perror("iosnandctl: private record/pidfd control refused"); return 1; }
    return 0;
usage:
    fprintf(stderr, "usage: iosnandctl status|stop|ready|clean|check-directory /private/root/directory/pid\n");
    return 2;
}
