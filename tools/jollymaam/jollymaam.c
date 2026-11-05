#define _POSIX_C_SOURCE 200809L
/* jollymaam: our pads' and dongles' firmware, on libjollymaam with this
 * platform's hooks (hooks_linux.c, hooks_win32.c or hooks_hidapi.c).
 * FIRMWARE is a package: a folder holding manifest.txt, or a zip of one
 * (jollymaam package makes both). */
#include "cli.h"
#include "hooks.h"
#include "../../src/lib/package.h"
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

static const char usage[] =
  "usage: jollymaam COMMAND ...\n"
  "  scan                         what is connected, by USB port\n"
  "  oemflash [OPTIONS] FIRMWARE  through the vendor loader (from our or stock firmware)\n"
  "  update   [OPTIONS] FIRMWARE  through our updater\n"
  "  oemloader [OPTIONS]          into the vendor loader, and wait there\n"
  "  start    [OPTIONS] [--firmware FIRMWARE]   out of it (Jieli: its RAM loader from FIRMWARE)\n"
  "  backup   [OPTIONS] [--firmware FIRMWARE] OUT   the whole flash, through the vendor loader\n"
  "  restore  [OPTIONS] [--firmware FIRMWARE] STOCK BACKUP_OUT   a stock image, after a backup\n"
  "  package  [--sdk JIELI_SDK] BUILD_DIR OUT[.zip]   a firmware package from a build\n"
  "  inspect  FILE                a package or an image\n"
  "  pack jieli|telink|join ...   build an image (as make does)\n"
  "  unpack   IMAGE DIR           a Jieli image's files\n"
  "OPTIONS: --device PORT (or node; default: every device that fits), --pid PID,\n"
  "         --wait SECONDS (for a device to appear; default 0)\n"
  "FIRMWARE: a folder with manifest.txt, or a zip of one.  Jieli ROM loaders need\n"
  "the RAM loaders a package carries (--firmware for start, backup, restore).\n";

/* ---- the hooks' output ------------------------------------------------------------------------- */

static void say(void *ctx, const char *line) {
  (void)ctx;
  printf("%s\n", line);
}

static void progress(void *ctx, const char *what, size_t done, size_t total) {
  static int last = -1;
  int pct = total ? (int)(done * 100 / total) : 100;
  (void)ctx;
  if (pct == last) return;
  last = pct;
  if (isatty(fileno(stderr))) fprintf(stderr, "\r%s %3d%%%s", what, pct, pct >= 100 ? "\n" : "");
  else if (pct % 25 == 0) fprintf(stderr, "%s %d%%\n", what, pct);
  if (pct >= 100) last = -1;
}

/* ---- firmware: a folder or a zip ----------------------------------------------------------------- */

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

static void open_firmware(const char *path, struct jm_firmware *fw) {
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

/* ---- commands ---------------------------------------------------------------------------------------- */

static int found(void *user, const struct jm_found *f) {
  static const char *const kinds[] = {"our firmware", "our updater", "Jieli ROM loader", "Jieli firmware",
                                      "8BitDo boot loader", "stock firmware"};
  char extra[80] = "";
  (void)user;
  if (f->kind == JM_FOUND_OURS || f->kind == JM_FOUND_UPDATER) snprintf(extra, sizeof extra, ", build %s", f->id.build);
  else if (f->kind == JM_FOUND_STOCK && f->mode[0])
    snprintf(extra, sizeof extra, ", %s mode%s", f->mode, f->can_enter ? "" : " (no loader command)");
  printf("%-24s %s, %s%s  (%s)\n", f->info.port[0] ? f->info.port : "-", kinds[f->kind],
         f->product ? f->product->pid : jm_chip_name(f->chip), extra, f->info.path);
  return 0;
}

static int to_file(void *user, uint32_t address, const uint8_t *data, size_t size) {
  (void)address;
  return fwrite(data, 1, size, *(FILE **)user) == size;
}
static int closed(void *user) {
  FILE **f = user;
  int ok = !fclose(*f);
  *f = NULL;
  return ok;
}

int main(int argc, char **argv) {
  struct jm_hooks hooks = {0};
  struct jm jm = {0};
  struct jm_select sel = {0};
  struct jm_firmware fw;
  const char *firmware = NULL, *rest[3] = {0};
  int nrest = 0;
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
    fputs(usage, argc < 2 ? stderr : stdout);
    return argc < 2 ? 2 : 0;
  }
  const char *cmd = argv[1];
  if (!strcmp(cmd, "pack")) return cmd_pack(argc - 2, argv + 2);
  if (!strcmp(cmd, "unpack")) return cmd_unpack(argc - 2, argv + 2);
  if (!strcmp(cmd, "package")) return cmd_package(argc - 2, argv + 2);
  if (!strcmp(cmd, "inspect")) {
    size_t size;
    if (argc != 3) die("usage: jollymaam inspect FILE");
    if (!is_dir(argv[2]) && inspect_image(argv[2], read_file(argv[2], &size), size)) return 0;
    open_firmware(argv[2], &fw);
    for (unsigned i = 0; i < fw.count; i++)
      printf("%-16s %-7s %s, %zu bytes\n", fw.files[i].name, fw.files[i].role, fw.files[i].path, fw.files[i].size);
    return 0;
  }
  static const struct { const char *name; int files; } commands[] = {
    {"scan", 0}, {"oemflash", 1}, {"update", 1}, {"oemloader", 0}, {"start", 0}, {"backup", 1}, {"restore", 2},
  };
  int known = -1;
  for (int i = 0; i < 7; i++)
    if (!strcmp(cmd, commands[i].name)) known = i;
  if (known < 0) die("no command %s (jollymaam help)", cmd);
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--device") && i + 1 < argc) sel.device = argv[++i];
    else if (!strcmp(argv[i], "--pid") && i + 1 < argc) sel.pid = argv[++i];
    else if (!strcmp(argv[i], "--wait") && i + 1 < argc) sel.wait_s = number(argv[++i]);
    else if (!strcmp(argv[i], "--firmware") && i + 1 < argc) firmware = argv[++i];
    else if (argv[i][0] != '-' && nrest < 3) rest[nrest++] = argv[i];
    else die("%s: unknown argument %s (jollymaam help)", cmd, argv[i]);
  }
  if (nrest != commands[known].files) die("%s: wrong arguments (jollymaam help)", cmd);
  if (sel.pid && !jm_product(sel.pid)) die("--pid: no product %s", sel.pid);
  if (known == 1 || known == 2) firmware = rest[0];
  if (firmware) open_firmware(firmware, &fw);
  hooks_device(&hooks);
  hooks.say = say, hooks.progress = progress;
  jm.h = &hooks;
  const struct jm_firmware *f = firmware ? &fw : NULL;
  switch (known) {
  case 0:
    if (!jm_scan(&jm, found, NULL)) puts("nothing of ours, no loader and no stock pad we know");
    return 0;
  case 1: return jm_oemflash(&jm, f, &sel) ? 1 : 0;
  case 2: return jm_update(&jm, f, &sel) ? 1 : 0;
  case 3: return jm_loader(&jm, &sel) ? 1 : 0;
  case 4: return jm_start(&jm, f, &sel) ? 1 : 0;
  }
  const char *out = rest[nrest - 1];
  FILE *file = fopen(out, "rb");
  if (file) fclose(file), die("%s: exists; a backup never overwrites", out);
  if (!(file = fopen(out, "wb"))) die("%s: cannot write", out);
  int r;
  if (known == 5) {
    r = jm_backup(&jm, f, &sel, to_file, &file);
  } else {
    size_t size;
    uint8_t *image = read_file(rest[0], &size);
    r = jm_restore(&jm, f, &sel, image, size, to_file, closed, &file);
  }
  if (file && !closed(&file)) r = -1;
  return r ? 1 : 0;
}
