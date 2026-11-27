// SPDX-License-Identifier: MIT
/*
 * vkeys: a virtual keyboard (uinput) playing a script of key presses, for
 * driving games on the iPad without hands (the desktop takes it as any
 * other keyboard).
 *
 *   vkeys STEP...
 *     wait:SECONDS        sleep
 *     tap:KEY             press and release
 *     down:KEY / up:KEY   hold / let go
 *   KEY: left right up down space enter esc ctrl alt tab, a-z, or a
 *   Linux key code number
 *
 *   vkeys wait:2 tap:enter wait:1 down:right wait:8 up:right
 */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fd;

static void
emit(int type, int code, int value)
{
   struct input_event ev = { .type = type, .code = code, .value = value };

   if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
      perror("vkeys: write");
}

static void
key(int code, int value)
{
   emit(EV_KEY, code, value);
   emit(EV_SYN, SYN_REPORT, 0);
}

static int
keycode(const char *k)
{
   static const struct { const char *n; int c; } m[] = {
      { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "up", KEY_UP }, { "down", KEY_DOWN },
      { "space", KEY_SPACE }, { "enter", KEY_ENTER }, { "esc", KEY_ESC }, { "ctrl", KEY_LEFTCTRL },
      { "alt", KEY_LEFTALT }, { "tab", KEY_TAB } };
   static const int letters[26] = { KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
                                    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
                                    KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };

   for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
      if (!strcmp(k, m[i].n))
         return m[i].c;
   if (k[0] >= 'a' && k[0] <= 'z' && !k[1])
      return letters[k[0] - 'a'];
   return atoi(k);
}

static void
pause_s(double s)
{
   struct timespec t = { (time_t)s, (long)((s - (time_t)s) * 1e9) };

   nanosleep(&t, NULL);
}

int
main(int argc, char **argv)
{
   struct uinput_setup us = { .id = { .bustype = BUS_USB, .vendor = 0x1234, .product = 0x5678 } };

   if (argc < 2) {
      fprintf(stderr, "usage: vkeys wait:S|tap:KEY|down:KEY|up:KEY ...\n");
      return 1;
   }
   if ((fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0) {
      perror("vkeys: /dev/uinput");
      return 1;
   }
   ioctl(fd, UI_SET_EVBIT, EV_KEY);
   for (int c = 1; c < 256; c++)
      ioctl(fd, UI_SET_KEYBIT, c);
   strcpy(us.name, "vkeys");
   ioctl(fd, UI_DEV_SETUP, &us);
   ioctl(fd, UI_DEV_CREATE);
   pause_s(1);                 /* (the desktop takes the new device) */
   for (int i = 1; i < argc; i++) {
      const char *a = argv[i], *v = strchr(a, ':');
      if (!v)
         continue;
      v++;
      if (!strncmp(a, "wait:", 5))
         pause_s(atof(v));
      else if (!strncmp(a, "tap:", 4)) {
         key(keycode(v), 1);
         pause_s(0.08);
         key(keycode(v), 0);
      } else if (!strncmp(a, "down:", 5))
         key(keycode(v), 1);
      else if (!strncmp(a, "up:", 3))
         key(keycode(v), 0);
   }
   pause_s(0.2);
   ioctl(fd, UI_DEV_DESTROY);
   close(fd);
   return 0;
}
