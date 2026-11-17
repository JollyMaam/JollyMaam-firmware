#define _POSIX_C_SOURCE 200809L
/* jollymaam: install, back up and restore firmware on our pads and dongles,
 * on libjollymaam with this platform's hooks (hooks_linux.c, hooks_win32.c
 * or hooks_hidapi.c): the help texts and the dispatch; the pad commands are
 * pads.c's, the building ones image.c's. */
#include "cli.h"
#include "hooks.h"
#include "../../src/lib/package.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

/* ---- the hooks' output ------------------------------------------------------------------------- */

static void say(void *ctx, const char *line) {
  (void)ctx;
  printf("%s\n", line);
}

/* A line per step, each percentage once: a new step (another name, or
 * back below the last percentage) starts it over. */
static void progress(void *ctx, const char *what, size_t done, size_t total) {
  static int last = -1;
  static const char *step;
  int pct = total ? (int)(done * 100 / total) : 100;
  (void)ctx;
  if (what != step || pct < last) step = what, last = -1;
  if (pct == last) return;
  last = pct;
  if (isatty(fileno(stderr))) fprintf(stderr, "\r%s %3d%%%s", what, pct, pct >= 100 ? "\n" : "");
  else if (pct % 25 == 0) fprintf(stderr, "%s %d%%\n", what, pct);
}

/* ---- a package: a folder or a zip ----------------------------------------------------------------- */

struct folder { const char *dir; };
static int folder_read(void *user, const char *path, const uint8_t **data, size_t *size) {
  struct folder *f = user;
  char full[1024];
  FILE *test;
  snprintf(full, sizeof full, "%s/%s", f->dir, path);
  if (!(test = fopen(full, "rb"))) return 0;
  fclose(test);
  *data = read_file(full, size);      /* kept for the run */
  return 1;
}

void open_firmware(const char *path, struct jm_firmware *fw) {
  static struct folder folder;
  static struct jm_package zip;
  char error[128];
  int ok;
  if (is_dir(path)) {
    folder.dir = path;
    ok = jm_firmware_open(fw, folder_read, &folder, error);
  } else {
    size_t size;
    uint8_t *data = read_file(path, &size);
    if (jm_package_open(&zip, data, size) != JM_PACKAGE_OK) die("%s: not a package (a folder or a zip)", path);
    ok = jm_firmware_open(fw, jm_zip_read, &zip, error);
  }
  if (!ok) die("%s: %s", path, error);
}

/* ---- help ------------------------------------------------------------------------------------------ */

static const char usage[] =
  "jollymaam: install, back up and restore firmware on JollyMaam pads and dongles\n"
  "\n"
  "usage: jollymaam [COMMAND] [OPTIONS] [ARGS]\n"
  "       jollymaam                  at a terminal: the installer, step by step\n"
  "       jollymaam FILE             a file dropped on the program: install FILE\n"
  "\n"
  "Pads:\n"
  "  list                       what is on USB: pads, their state and USB port\n"
  "  install [FILE]             put firmware on a pad: ours (a package, the default),\n"
  "                             a backup, or a vendor image; backs the whole flash\n"
  "                             up first\n"
  "  update                     our firmware, through our updater (keeps everything,\n"
  "                             no boot loader needed)\n"
  "  backup [OUT]               the whole flash into a file\n"
  "  reboot                     restart a pad; --loader: into its boot loader, and stay\n"
  "\n"
  "Building:\n"
  "  package BUILD_DIR OUT      a package (OUT.zip or a folder) from a build\n"
  "  inspect FILE               what a package or an image holds\n"
  "  pack jieli|telink|join     build an image (as make does)\n"
  "  unpack IMAGE DIR           a Jieli image's files\n"
  "\n"
  "Options for pad commands:\n"
  "  --device PORT              the pad on this USB port (as list shows)\n"
  "  --product PID              only pads of this product (list --products)\n"
  "  --package PACKAGE          our firmware and the Jieli flash helpers\n"
  "                             (default: firmware.zip here, else beside the program\n"
  "                             or a folder up from it)\n"
  "  --wait SECONDS             for a pad to appear (default: at a terminal until\n"
  "                             one does, else 0)\n"
  "  --yes                      no questions (several pads: install takes the first\n"
  "                             on stock firmware; else --device is needed)\n"
  "\n"
  "jollymaam help COMMAND, or jollymaam COMMAND --help: more about a command.\n"
  "A PACKAGE is a folder with manifest.txt, or a zip of one (make firmware\n"
  "builds build/firmware.zip).\n";

#define PAD_OPTIONS "  --device, --product, --package, --wait, --yes   as for every pad command\n"

static const struct { const char *name, *help; } helps[] = {
  {"list",
   "usage: jollymaam list [--products]\n"
   "\n"
   "What is on USB, a line each: its USB port, what runs on it (our firmware,\n"
   "our updater, a boot loader, stock firmware) and its product.\n"
   "\n"
   "  --products     the products jollymaam knows instead: PID, name, chip\n"},
  {"install",
   "usage: jollymaam install [OPTIONS] [FILE]\n"
   "\n"
   "Puts firmware on a pad, through its boot loader: a running pad is restarted\n"
   "into it first. The pad's whole flash is backed up into a file first, unless\n"
   "--no-backup.\n"
   "\n"
   "FILE is one of:\n"
   "  a package (default: --package's)   our firmware: updater and app; the\n"
   "                                     settings start empty\n"
   "  a backup (from install or backup)  of this pad, stock or ours: written back\n"
   "                                     whole, settings and pairings included\n"
   "  a vendor image (raw or .ufw)       stock firmware; its settings start empty\n"
   "\n"
   "Whatever FILE is, the pad's factory data (Bluetooth address, radio trim) is\n"
   "left alone, unless --raw. A backup of another pad, a file for another\n"
   "product, or one for another chip key is refused before anything is written.\n"
   "\n"
   "Options:\n"
   "  --no-backup     skip the backup\n"
   "  --backup OUT    the backup's file (default: PID-DATE-TIME.bin here; never\n"
   "                  overwritten)\n"
   "  --raw           FILE a whole-flash backup (any pad's): written exactly as it\n"
   "                  is, factory data included\n"
   PAD_OPTIONS
   "\n"
   "8BitDo pads: a backup can't be restored yet; their stock app can be (U2CW: a\n"
   "raw app or .dat; U2CB: v1.01, or a backup holding it).\n"
   "\n"
   "Examples:\n"
   "  jollymaam install                            ours, from firmware.zip\n"
   "  jollymaam install GS_C2_ADC_DEVICE-20261111-130700.bin\n"
   "  jollymaam install --no-backup --yes --device 3-1 build/firmware.zip\n"},
  {"update",
   "usage: jollymaam update [OPTIONS]\n"
   "\n"
   "Our firmware's app from the package, through our updater: the pad restarts\n"
   "into it, takes the app and starts it. Settings, pairings and the updater stay;\n"
   "no boot loader, no backup. Only for pads running our firmware (or its updater).\n"
   "\n"
   "Options:\n"
   PAD_OPTIONS},
  {"backup",
   "usage: jollymaam backup [OPTIONS] [OUT]\n"
   "\n"
   "The whole flash, read through the boot loader, into OUT (default:\n"
   "PID-DATE-TIME.bin here; never overwritten). The pad is left as it was\n"
   "found: a running pad is restarted back into its firmware, one in its boot\n"
   "loader stays there.\n"
   "\n"
   "Options:\n"
   "  --stay          a running pad stays in its boot loader afterwards\n"
   PAD_OPTIONS},
  {"reboot",
   "usage: jollymaam reboot [OPTIONS]\n"
   "\n"
   "Restarts a pad: out of its boot loader the firmware it holds starts; our\n"
   "firmware restarts.\n"
   "\n"
   "Options:\n"
   "  --loader        into its boot loader instead (from our or stock firmware),\n"
   "                  and it stays there\n"
   PAD_OPTIONS},
  {"package",
   "usage: jollymaam package [--sdk JIELI_SDK] BUILD_DIR OUT[.zip]\n"
   "\n"
   "A package from a build directory: each board's install image and update.bin,\n"
   "and the Jieli RAM loaders (from JIELI_SDK). OUT.zip is a zip, else a folder.\n"},
  {"inspect",
   "usage: jollymaam inspect FILE\n"
   "\n"
   "What FILE holds: a package's files, or an image's layout.\n"},
  {"pack",
   "usage: jollymaam pack jieli|telink|join ...\n"
   "\n"
   "Builds an image, as make does; jollymaam pack KIND --help for its arguments.\n"},
  {"unpack",
   "usage: jollymaam unpack IMAGE DIR\n"
   "\n"
   "A Jieli image's files into DIR.\n"},
};

/* COMMAND's help to stdout (2: no such command). */
static int help(const char *command) {
  if (!command) return fputs(usage, stdout), 0;
  for (size_t i = 0; i < sizeof helps / sizeof helps[0]; i++)
    if (!strcmp(command, helps[i].name)) return fputs(helps[i].help, stdout), 0;
  fprintf(stderr, "jollymaam: no command %s (jollymaam help)\n", command);
  return 2;
}

void usage_error(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("jollymaam: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputs(" (jollymaam help)\n", stderr);
  va_end(ap);
  exit(2);
}

/* ---- the dispatch ---------------------------------------------------------------------------------- */

static int is_help(const char *a) { return !strcmp(a, "--help") || !strcmp(a, "-h"); }

/* The window a double-click opened stays until Enter. */
static int keep_open(int r) {
  char line[8];
  printf("\nPress Enter to close.");
  fflush(stdout);
  if (!fgets(line, sizeof line, stdin)) line[0] = 0;
  return r;
}

int main(int argc, char **argv) {
  struct jm_hooks hooks = {0};
  struct jm jm = {0};
  setvbuf(stdout, NULL, _IOLBF, 0);
  hooks_device(&hooks);
  hooks.say = say, hooks.progress = progress;
  jm.h = &hooks;
  if (argc < 2) {
    if (isatty(fileno(stdin)) && isatty(fileno(stdout))) return keep_open(cmd_pads("install", 0, argv + 1, argv[0], &jm));
    fputs(usage, stderr);
    return 2;
  }
  const char *cmd = argv[1];
  if (!strcmp(cmd, "help") || is_help(cmd)) return help(argc > 2 && !is_help(cmd) ? argv[2] : NULL);
  for (int i = 2; i < argc; i++)
    if (is_help(argv[i]) && strcmp(cmd, "pack")) return help(cmd);
  if (!strcmp(cmd, "pack")) return cmd_pack(argc - 2, argv + 2);
  if (!strcmp(cmd, "unpack")) return cmd_unpack(argc - 2, argv + 2);
  if (!strcmp(cmd, "package")) return cmd_package(argc - 2, argv + 2);
  if (!strcmp(cmd, "inspect")) {
    struct jm_firmware fw;
    size_t size;
    if (argc != 3) usage_error("usage: jollymaam inspect FILE");
    if (!is_dir(argv[2]) && inspect_image(argv[2], read_file(argv[2], &size), size)) return 0;
    open_firmware(argv[2], &fw);
    for (unsigned i = 0; i < fw.count; i++)
      printf("%-16s %-7s %s, %zu bytes\n", fw.files[i].name, fw.files[i].role, fw.files[i].path, fw.files[i].size);
    return 0;
  }
  static const char *const pads[] = {"list", "install", "update", "backup", "reboot"};
  for (size_t i = 0; i < sizeof pads / sizeof pads[0]; i++)
    if (!strcmp(cmd, pads[i])) return cmd_pads(cmd, argc - 2, argv + 2, argv[0], &jm);
  /* A file dropped on the program (Windows passes its path): installed. */
  FILE *dropped = argc == 2 && cmd[0] != '-' && strpbrk(cmd, "/\\.") ? fopen(cmd, "rb") : NULL;
  if (dropped || (argc == 2 && cmd[0] != '-' && strpbrk(cmd, "/\\.") && is_dir(cmd))) {
    if (dropped) fclose(dropped);
    return keep_open(cmd_pads("install", 1, argv + 1, argv[0], &jm));
  }
  usage_error("no command %s", cmd);
  return 2;
}
