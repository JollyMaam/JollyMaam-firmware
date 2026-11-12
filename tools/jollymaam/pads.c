#define _POSIX_C_SOURCE 200809L
/* jollymaam's pad commands: list, install, update, backup, reboot (help
 * texts in jollymaam.c).  Each acts on one pad: the one --device and
 * --product leave, waited for, or of several the one the person picks
 * (with --yes: install takes the first on stock firmware).  The steps are
 * libjollymaam's jm_pad: one boot loader session, the Jieli RAM loader
 * uploaded once. */
#include "cli.h"
#include "../../src/lib/jollymaam.h"
#include "../../src/lib/package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
static void nap_s(unsigned s) { Sleep(s * 1000); }
#else
#include <unistd.h>
static void nap_s(unsigned s) { sleep(s); }
#endif

#define PADS 16

struct options {
  const char *command, *device, *product, *package, *backup_out, *args[2];
  int nargs, wait, yes, no_backup, raw, stay, loader, products;
};

static int interactive(void) { return isatty(fileno(stdin)) && isatty(fileno(stdout)); }

/* A line from the person ("" at the end of input). */
static void ask(const char *question, char *answer, size_t room) {
  printf("%s", question);
  fflush(stdout);
  if (!fgets(answer, (int)room, stdin)) answer[0] = 0;
  answer[strcspn(answer, "\r\n")] = 0;
}

/* Yes to question; empty: the default. */
static int yes(const char *question, int fallback) {
  char a[16];
  ask(question, a, sizeof a);
  return a[0] ? a[0] == 'y' || a[0] == 'Y' : fallback;
}

/* ---- the pad ------------------------------------------------------------------------------------- */

struct pads { struct jm_found f[PADS]; int n; const struct options *o; int ours_only; };
static int collect(void *user, const struct jm_found *f) {
  struct pads *p = user;
  const struct options *o = p->o;
  if (f->kind == JM_FOUND_JIELI || p->n >= PADS) return 0;
  if (p->ours_only && f->kind != JM_FOUND_OURS && f->kind != JM_FOUND_UPDATER) return 0;
  if (o->device && strcmp(o->device, f->info.path) && strcmp(o->device, f->info.port)) return 0;
  /* A Jieli loader's product is known once its flash is read: kept. */
  if (o->product && f->product && strcmp(o->product, f->product->pid)) return 0;
  p->f[p->n++] = *f;
  return 0;
}

static const char *name_of(const struct jm_found *f) {
  return f->product ? f->product->name : "A Jieli pad or dongle";
}

static void describe(int index, const struct jm_found *f) {
  char extra[96] = "";
  if (f->kind == JM_FOUND_OURS || f->kind == JM_FOUND_UPDATER) snprintf(extra, sizeof extra, ", build %s", f->id.build);
  else if (f->kind == JM_FOUND_STOCK && f->mode[0])
    snprintf(extra, sizeof extra, ", %s mode%s", f->mode, f->can_enter ? "" : " (no boot loader command)");
  else if (f->kind == JM_FOUND_JIELI_LOADER) snprintf(extra, sizeof extra, " (%s)", jm_chip_name(f->chip));
  if (index) printf("  %d. ", index);
  printf("%s: %s%s  [%s]\n", name_of(f), jm_found_what(f), extra, f->info.port[0] ? f->info.port : f->info.path);
}

/* The pad to act on, waited for (wait < 0: until one appears); 0 with a
 * message when there is none, or several and no way to choose. */
static int choose(struct jm *jm, const struct options *o, int ours_only, struct jm_found *out) {
  struct pads p;
  for (int t = 0;; t++) {
    memset(&p, 0, sizeof p);
    p.o = o, p.ours_only = ours_only;
    jm_scan(jm, collect, &p);
    if (p.n) break;
    if (o->wait >= 0 && t >= o->wait) {
      fprintf(stderr, "jollymaam: no %s on USB%s (jollymaam list)\n", ours_only ? "pad running our firmware" : "pad",
              o->device ? " there" : "");
      return 0;
    }
    if (!t) printf("Plug in your pad over USB%s...\n", o->device ? " there" : "");
    nap_s(1);
  }
  int pick = 0, stock = -1;
  for (int i = 0; i < p.n && stock < 0; i++)
    if (p.f[i].kind == JM_FOUND_STOCK) stock = i;
  if (p.n > 1) {
    if (o->yes || !interactive()) {
      if (strcmp(o->command, "install") || stock < 0) {
        fprintf(stderr, "jollymaam: %d pads on USB: pick one with --device PORT (jollymaam list)\n", p.n);
        return 0;
      }
      pick = stock;
    } else {
      char a[16];
      pick = stock >= 0 && !strcmp(o->command, "install") ? stock : 0;
      printf("%d pads on USB:\n", p.n);
      for (int i = 0; i < p.n; i++) describe(i + 1, &p.f[i]);
      for (;;) {
        printf("Which one? [%d] ", pick + 1);
        ask("", a, sizeof a);
        if (!a[0]) break;
        int k = atoi(a);
        if (k >= 1 && k <= p.n) { pick = k - 1; break; }
      }
    }
  }
  *out = p.f[pick];
  return 1;
}

/* The package into fw: given, else firmware.zip here, beside the program
 * or one folder up (build/host/
 * jollymaam finds build/firmware.zip).  What it is, for messages; NULL (fw
 * empty) when there is none. */
static const char *load_package(const char *given, const char *program, struct jm_firmware *fw) {
  static char path[1024];
  FILE *f;
  memset(fw, 0, sizeof *fw);
  if (given) return open_firmware(given, fw), given;
  if ((f = fopen("firmware.zip", "rb"))) return fclose(f), open_firmware("firmware.zip", fw), "firmware.zip";
  const char *slash = strrchr(program, '/'), *back = strrchr(program, '\\');
  if (back > slash) slash = back;
  for (int up = 0; slash && up < 2; up++) {
    snprintf(path, sizeof path, "%.*s%sfirmware.zip", (int)(slash - program + 1), program, up ? "../" : "");
    if ((f = fopen(path, "rb"))) return fclose(f), open_firmware(path, fw), path;
  }
  return NULL;
}

#define NO_PACKAGE "no firmware.zip here or beside the program: name a package (--package PACKAGE)"

/* FILE a package (a folder, or a zip that opens as one)? */
static int is_package(const char *path) {
  struct jm_package zip;
  struct jm_firmware fw;
  char error[128];
  size_t size;
  if (is_dir(path)) return 1;
  uint8_t *data = read_file(path, &size);
  int ok = jm_package_open(&zip, data, size) == JM_PACKAGE_OK && jm_firmware_open(&fw, jm_zip_read, &zip, error);
  free(data);
  return ok;
}

/* ---- backups ------------------------------------------------------------------------------------- */

struct backup { FILE *file; char path[512]; };
static int to_backup(void *user, uint32_t address, const uint8_t *data, size_t size) {
  struct backup *b = user;
  (void)address;
  return fwrite(data, 1, size, b->file) == size;
}

/* The whole flash into out (NULL: PID-DATE-TIME.bin here), never over a
 * file; 0 with a message on failure (the file removed). */
static int save_backup(struct jm_pad *pad, const char *out) {
  struct backup b;
  if (out) snprintf(b.path, sizeof b.path, "%s", out);
  else {
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(b.path, sizeof b.path, "%s-%s.bin", pad->product[0] ? pad->product : "pad", stamp);
  }
  if ((b.file = fopen(b.path, "rb"))) {
    fclose(b.file);
    fprintf(stderr, "jollymaam: %s exists; a backup never overwrites\n", b.path);
    return 0;
  }
  if (!(b.file = fopen(b.path, "wb"))) {
    fprintf(stderr, "jollymaam: %s: cannot write\n", b.path);
    return 0;
  }
  int ok = jm_pad_backup(pad, to_backup, &b);
  if (fclose(b.file)) ok = 0;
  if (!ok) {
    remove(b.path);
    puts("The backup failed.");
    return 0;
  }
  printf("Backed up: %s\n", b.path);
  return 1;
}

/* ---- the commands -------------------------------------------------------------------------------- */

static int list_found(void *user, const struct jm_found *f) {
  (void)user;
  describe(0, f);
  return 0;
}

static int cmd_list(struct jm *jm, const struct options *o) {
  if (o->products) {
    const struct jm_product *p;
    for (size_t i = 0; (p = jm_product_at(i)); i++)
      printf("%-18s %-24s %s%s\n", p->pid, p->name, jm_chip_name(p->chip), p->board ? "" : "  (no firmware of ours)");
    return 0;
  }
  if (!jm_scan(jm, list_found, NULL)) puts("No pad on USB that jollymaam knows.");
  return 0;
}

static int cmd_install(struct jm *jm, const struct options *o, const char *program) {
  const char *file = o->nargs ? o->args[0] : NULL, *package = o->package;
  uint8_t *image = NULL;
  size_t image_size = 0;
  if (file && is_package(file)) {
    if (package) usage_error("install: two packages, %s and --package %s", file, package);
    if (o->raw) usage_error("install --raw: FILE is a package; --raw takes a whole-flash backup");
    package = file;
  } else if (file) {
    image = read_file(file, &image_size);
  } else if (o->raw) {
    usage_error("install --raw: name the backup (FILE)");
  }
  struct jm_firmware fw;
  if (!(package = load_package(package, program, &fw))) die(NO_PACKAGE);
  if (!o->yes && !interactive()) die("install: not at a terminal; --yes to go ahead without asking");

  struct jm_found found;
  if (!choose(jm, o, 0, &found)) return 1;
  describe(0, &found);
  if (found.kind == JM_FOUND_STOCK && !found.can_enter) {
    printf("It has no command for its boot loader in this mode.%s%s\n",
           found.product && found.product->by_hand ? " Into it: " : "",
           found.product && found.product->by_hand ? found.product->by_hand : "");
    return 1;
  }
  if (!image && found.product && !jm_firmware_find(&fw, found.product->pid, "install")) {
    printf("%s has no firmware for it.\n", package);
    return 1;
  }
  if (!o->yes) {
    char question[700];
    const char *first = o->no_backup ? "Without a backup, " : "Back up its whole flash into a file here, then ";
    if (image)
      snprintf(question, sizeof question, "%s%s %s onto it? [y/N] ", first,
               o->raw ? "write all of" : "restore", file);
    else
      snprintf(question, sizeof question, "%sinstall JollyMaam (its settings start empty)? [Y/n] ", first);
    if (!yes(question, !image)) return 1;
  }

  struct jm_pad pad;
  jm_pad_init(&pad, jm, &fw, &found);
  if (!jm_pad_ready(&pad)) return 1;
  if (!pad.product[0]) {
    puts("Its flash names no product (blank?): leaving it alone.");
    return 1;
  }
  if (!image && !jm_firmware_find(&fw, pad.product, "install")) {
    printf("%s has no firmware for %s.\n", package, pad.product);
    return 1;
  }
  if (!o->no_backup && !save_backup(&pad, o->backup_out)) {
    puts("Nothing was written to the pad. It stays in its boot loader.");
    return 1;
  }
  if (image) {
    if (!jm_pad_restore(&pad, image, image_size, o->raw)) {
      puts("Not written: the pad stays in its boot loader (jollymaam reboot, or unplug it).");
      return 1;
    }
    printf("Done: %s now holds %s.\n", found.product ? found.product->name : pad.product, file);
    return 0;
  }
  if (!jm_pad_install(&pad)) {
    puts("The install failed: the pad stays in its boot loader. Run this again to retry.");
    return 1;
  }
  /* Its first start: our firmware on USB again. */
  struct options again = *o;
  again.device = found.info.port[0] ? found.info.port : NULL;
  again.product = pad.product;
  for (int t = 0; t < 15; t++) {
    struct pads p;
    memset(&p, 0, sizeof p);
    p.o = &again, p.ours_only = 1;
    nap_s(1);
    jm_scan(jm, collect, &p);
    if (p.n) {
      printf("Done: %s runs JollyMaam, build %s.\n", name_of(&p.f[0]), p.f[0].id.build);
      return 0;
    }
  }
  puts("Written and checked; the pad did not show up on USB again yet (in another USB mode, or unplugged).");
  return 0;
}

static int cmd_update(struct jm *jm, const struct options *o, const char *program) {
  struct jm_firmware fw;
  struct jm_found found;
  if (!load_package(o->package, program, &fw)) die(NO_PACKAGE);
  if (!choose(jm, o, 1, &found)) return 1;
  describe(0, &found);
  struct jm_select sel = {found.info.path, NULL, 30, 0};
  return jm_update(jm, &fw, &sel) ? 1 : 0;
}

static int cmd_backup(struct jm *jm, const struct options *o, const char *program) {
  struct jm_firmware fw;
  struct jm_found found;
  struct jm_pad pad;
  load_package(o->package, program, &fw);
  if (!choose(jm, o, 0, &found)) return 1;
  describe(0, &found);
  int running = found.kind != JM_FOUND_JIELI_LOADER && found.kind != JM_FOUND_8BITDO_LOADER;
  jm_pad_init(&pad, jm, &fw, &found);
  if (!jm_pad_ready(&pad) || !save_backup(&pad, o->nargs ? o->args[0] : NULL)) return 1;
  if (running && !o->stay) return jm_pad_reboot(&pad) ? 0 : 1;
  puts("It stays in its boot loader (jollymaam reboot).");
  return 0;
}

static int cmd_reboot(struct jm *jm, const struct options *o, const char *program) {
  struct jm_firmware fw;
  struct jm_found found;
  struct jm_pad pad;
  load_package(o->package, program, &fw);
  if (!choose(jm, o, 0, &found)) return 1;
  describe(0, &found);
  if (o->loader) {
    if (found.kind == JM_FOUND_JIELI_LOADER || found.kind == JM_FOUND_8BITDO_LOADER) {
      puts("It is in its boot loader already.");
      return 0;
    }
    struct jm_select sel = {found.info.path, NULL, 30, 0};
    return jm_loader(jm, &sel) ? 1 : 0;
  }
  jm_pad_init(&pad, jm, &fw, &found);
  return jm_pad_reboot(&pad) ? 0 : 1;
}

/* ---- options ------------------------------------------------------------------------------------- */

int cmd_pads(const char *command, int argc, char **argv, const char *program, struct jm *jm) {
  struct options o;
  memset(&o, 0, sizeof o);
  o.command = command;
  o.wait = interactive() ? -1 : 0;
  int pad_options = strcmp(command, "list") != 0;
  for (int i = 0; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define FLAG(name, cond) (!strcmp(a, name) && ((cond) || (usage_error("%s takes no %s", command, name), 0)))
#define VALUE(name, cond) (FLAG(name, cond) && (v || (usage_error("%s: %s needs a value", command, name), 0)) && ++i)
    if (VALUE("--device", pad_options)) o.device = v;
    else if (VALUE("--product", pad_options)) o.product = v;
    else if (VALUE("--package", pad_options)) o.package = v;
    else if (VALUE("--wait", pad_options)) o.wait = (int)number(v);
    else if (FLAG("--yes", pad_options)) o.yes = 1;
    else if (FLAG("--no-backup", !strcmp(command, "install"))) o.no_backup = 1;
    else if (VALUE("--backup", !strcmp(command, "install"))) o.backup_out = v;
    else if (FLAG("--raw", !strcmp(command, "install"))) o.raw = 1;
    else if (FLAG("--stay", !strcmp(command, "backup"))) o.stay = 1;
    else if (FLAG("--loader", !strcmp(command, "reboot"))) o.loader = 1;
    else if (FLAG("--products", !strcmp(command, "list"))) o.products = 1;
    else if (a[0] == '-' && a[1]) usage_error("%s: no option %s", command, a);
    else if (o.nargs < 1 && (!strcmp(command, "install") || !strcmp(command, "backup"))) o.args[o.nargs++] = a;
    else usage_error("%s: too many arguments (%s)", command, a);
#undef VALUE
#undef FLAG
  }
  if (o.product && !jm_product(o.product)) usage_error("--product: no product %s (jollymaam list --products)", o.product);
  if (o.no_backup && o.backup_out) usage_error("install: --no-backup and --backup together");
  if (!strcmp(command, "list")) return cmd_list(jm, &o);
  if (!strcmp(command, "install")) return cmd_install(jm, &o, program);
  if (!strcmp(command, "update")) return cmd_update(jm, &o, program);
  if (!strcmp(command, "backup")) return cmd_backup(jm, &o, program);
  return cmd_reboot(jm, &o, program);
}
