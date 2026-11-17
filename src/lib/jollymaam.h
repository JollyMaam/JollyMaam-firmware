#ifndef JOLLYMAAM_H
#define JOLLYMAAM_H
/* libjollymaam: our pads' and dongles' firmware, images and loaders.
 *
 * The library knows the products (products.c), reads firmware (a manifest
 * and its files, firmware.c) and drives every device operation: finding
 * devices by USB port, our firmware's tool commands and updater, the vendor
 * loaders (Jieli ROM, 8BitDo Telink and Yichip), stock firmware's loader
 * entry (device.c).  The application supplies device access as hooks: one
 * table per platform (the jollymaam CLI's Linux, Windows and hidapi ones,
 * Android's JNI bridge).  Nothing here opens files or devices itself. */
#include <stddef.h>
#include <stdint.h>
#include "proto_jollymaam.h"

/* ---- products --------------------------------------------------------------------- */

enum jm_chip { JM_CHIP_BR23, JM_CHIP_BD19, JM_CHIP_TLSR8208, JM_CHIP_YC3121 };
struct jm_product {
  const char *pid;              /* the image's PID: IDENTIFY's, the manifest's */
  const char *board;            /* its build directory under build/ (NULL: no firmware of ours) */
  const char *name;             /* for people */
  enum jm_chip chip;
  uint16_t model;               /* the 8BitDo boot loader's model; 0: none */
  const char *install;          /* what the vendor loader writes, in its build directory */
  int update;                   /* has update.bin, for our updater */
  uint32_t factory_offset, factory_size;   /* this unit's factory data (Jieli: BTIF, key_mac and
                                            * what lies between): never erased but by a raw write;
                                            * 0: not established (an install writes its image only,
                                            * a restore takes a raw write) */
  const char *by_hand;          /* how to enter the vendor loader by hand */
};
const struct jm_product *jm_product(const char *pid);
const struct jm_product *jm_product_at(size_t index);   /* NULL past the last */
const char *jm_chip_name(enum jm_chip chip);

/* Stock firmware as USB shows it: a HID interface by its IDs (and strings,
 * where modes share IDs).  With a usage page, the interface that takes the
 * command restarting it into its vendor loader (report: its ID byte, then
 * the command, zero-padded to len); without, a mode that has none.  model:
 * 8BitDo stock, whose product is the boot loader model it answers on that
 * interface (output report 0x81). */
struct jm_stock {
  const char *pid, *mode;       /* pid NULL: by model */
  uint16_t vid, product, bcd;   /* bcd 0: any */
  const char *manufacturer, *product_name;   /* NULL: any */
  uint16_t usage_page;
  uint8_t len, report[5], model;
};
const struct jm_stock *jm_stock_at(size_t index);

/* ---- firmware: a manifest and its files --------------------------------------------
 * manifest.txt, its first line "jollymaam-firmware 2", then a line a file:
 *   NAME ROLE PATH SIZE SHA256
 * NAME is a product's PID (ROLE install or update) or a Jieli chip (ROLE
 * loader: the SDK's RAM loader for it).  The files come through read, from a
 * zip (jm_zip_read) or a folder (the application's). */
#define JM_FIRMWARE_FILES 32
struct jm_firmware_file {
  char name[17], role[8], path[96];
  const uint8_t *data;
  size_t size;
};
struct jm_firmware {
  unsigned count;
  struct jm_firmware_file files[JM_FIRMWARE_FILES];
};
/* A file of the firmware: 1 with data borrowed until the firmware is done
 * with, 0 when there is none. */
typedef int (*jm_read)(void *user, const char *path, const uint8_t **data, size_t *size);
int jm_firmware_open(struct jm_firmware *fw, jm_read read, void *user, char error[128]);
const struct jm_firmware_file *jm_firmware_find(const struct jm_firmware *fw, const char *name,
                                                const char *role);
/* A manifest line for a file, into out (at most 200 bytes with its newline). */
int jm_firmware_line(char *out, size_t room, const char *name, const char *role, const char *path,
                     const uint8_t *data, size_t size);
/* read over a zip in memory (user: its struct jm_package, package.h). */
int jm_zip_read(void *package, const char *path, const uint8_t **data, size_t *size);

/* ---- hooks: the application's device access ----------------------------------------- */

enum jm_device_kind { JM_DEVICE_HID, JM_DEVICE_DISK };
struct jm_device_info {
  enum jm_device_kind kind;
  char path[300];               /* what open takes */
  char port[64];                /* its USB port; "" if not known */
  char id[96];                  /* changes with every enumeration (port and device number) */
  uint16_t vid, pid, bcd, usage_page;
  uint16_t feature_size, output_size;   /* HID: its largest reports, without the ID byte */
  char manufacturer[64], product[64];
};
enum jm_scsi_direction { JM_SCSI_NONE, JM_SCSI_IN, JM_SCSI_OUT };
/* From a HID report descriptor: its first usage page and its largest
 * feature and output reports in bytes (for hooks that read descriptors). */
void jm_hid_descriptor(const uint8_t *d, size_t n, struct jm_device_info *info);
struct jm_hooks {
  void *ctx;
  /* Every HID interface (top-level collection) and USB disk; visit returns
   * nonzero to stop.  The count, or -1. */
  int (*list)(void *ctx, int (*visit)(void *user, const struct jm_device_info *info), void *user);
  void *(*open)(void *ctx, const struct jm_device_info *info);
  void (*close)(void *ctx, void *dev);
  /* HID reports: write and set_feature take the report ID byte first (0:
   * unnumbered), read gives a report as hidraw does (its ID byte only when
   * numbered): its length, 0 after timeout_ms, -1 on an error. */
  int (*write)(void *ctx, void *dev, const uint8_t *report, size_t size);
  int (*read)(void *ctx, void *dev, uint8_t *out, size_t size, unsigned timeout_ms);
  int (*set_feature)(void *ctx, void *dev, const uint8_t *report, size_t size);
  /* A SCSI command on a disk: 0 when it completed with good status, the
   * bytes moved in *done.  NULL: no disks here (no Jieli ROM loaders). */
  int (*scsi)(void *ctx, void *dev, const uint8_t *cdb, size_t cdb_size, enum jm_scsi_direction direction,
              uint8_t *data, size_t size, unsigned timeout_ms, size_t *done);
  void (*sleep)(void *ctx, unsigned ms);
  void (*say)(void *ctx, const char *line);   /* what happens, a line at a time */
  void (*progress)(void *ctx, const char *what, size_t done, size_t total);   /* optional */
};

/* ---- devices ----------------------------------------------------------------------------- */

enum jm_found_kind {
  JM_FOUND_OURS,                /* our firmware: its tool device */
  JM_FOUND_UPDATER,             /* our updater */
  JM_FOUND_JIELI_LOADER,        /* a Jieli ROM loader (product unknown until its RAM loader runs) */
  JM_FOUND_JIELI,               /* Jieli firmware we do not know (a USB disk) */
  JM_FOUND_8BITDO_LOADER,       /* the 8BitDo boot loader */
  JM_FOUND_STOCK,               /* stock firmware */
};
struct jm_found {
  enum jm_found_kind kind;
  struct jm_device_info info;
  const struct jm_product *product;   /* NULL: not known */
  enum jm_chip chip;
  const char *mode;             /* stock: its USB mode; "" */
  int can_enter;                /* a command restarts it into its loader */
  struct jm_identity id;        /* ours and our updater */
};

/* The library's state for one application; zero it, then set hooks. */
struct jm {
  const struct jm_hooks *h;
  char asked[16][96];           /* asked to restart: left alone until they leave the bus */
  unsigned n_asked;
  uint32_t client;
  uint8_t tag;
};

typedef int (*jm_found_visit)(void *user, const struct jm_found *found);
int jm_scan(struct jm *jm, jm_found_visit visit, void *user);

/* What an operation applies to: device a port or path (NULL: every device
 * that fits), pid a product (NULL: any), wait_s how long to wait for one;
 * ask_only: a restart into a loader is asked for, not waited for (the
 * application looks for the loader itself). */
struct jm_select { const char *device, *pid; unsigned wait_s; int ask_only; };

/* Through the vendor loader: running firmware (ours or stock) is restarted
 * into it, the firmware's install image written, read back and started. */
int jm_oemflash(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel);
/* Through our updater: the firmware's update.bin. */
int jm_update(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel);
/* Into the vendor loader and wait there; out of it (the app starts).  A
 * Jieli loader needs the firmware's RAM loader for start (fw may be NULL
 * otherwise). */
int jm_loader(struct jm *jm, const struct jm_select *sel);
int jm_start(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel);
/* The whole flash through the vendor loader, a block at a time to sink. */
typedef int (*jm_sink)(void *user, uint32_t address, const uint8_t *data, size_t size);
int jm_backup(struct jm *jm, const struct jm_firmware *fw, const struct jm_select *sel,
              jm_sink sink, void *user);
/* ---- a pad, step by step ------------------------------------------------------------------
 * One device (from jm_scan) taken through an installer's steps, followed
 * on its port through its restarts.  The first step that needs the vendor
 * loader restarts the pad into it and readies it: a Jieli ROM loader gets
 * its RAM loader (from fw) once, for every later step, and the product its
 * flash names is read.  Each step: 1 done, 0 failed (said why). */
struct jm_pad {
  struct jm *jm;
  const struct jm_firmware *fw;   /* the Jieli RAM loaders, the install images */
  struct jm_found found;          /* what it is (after a restart: what it was) */
  int ready;                      /* in its vendor loader, readied */
  int gone;                       /* restarted out of it (installed or rebooted) */
  char product[17];               /* ready: the product it holds ("": not known) */
  struct { uint32_t flash_id, flash_size, io; uint16_t key; } helper;   /* Jieli: the RAM loader's */
};
void jm_pad_init(struct jm_pad *pad, struct jm *jm, const struct jm_firmware *fw, const struct jm_found *found);
int jm_pad_ready(struct jm_pad *pad);
int jm_pad_backup(struct jm_pad *pad, jm_sink sink, void *user);   /* the whole flash */
/* The package's install image, read back and started; on a Jieli pad the
 * rest of the flash erased but the factory data (settings start empty). */
int jm_pad_install(struct jm_pad *pad);
/* Out of the loader the firmware it holds starts; our firmware restarts. */
int jm_pad_reboot(struct jm_pad *pad);
/* Back to what the image holds, read back and started.  Jieli: a whole-flash
 * backup of this pad (from jm_pad_backup, stock or our firmware), all but
 * its factory data written back (a backup of another pad is refused); or a
 * vendor image (raw or UFW), written at 0, the rest erased but the factory
 * data.  raw: a whole-flash backup (any pad's) written as it is, factory
 * data included.  The file must name the product the pad holds and fit its
 * chip key.  8BitDo: its stock app (U2CW raw or DAT, U2CB v1.01). */
int jm_pad_restore(struct jm_pad *pad, const uint8_t *image, size_t size, int raw);
/* What a device found is, for people ("our firmware", "Jieli ROM loader", ...). */
const char *jm_found_what(const struct jm_found *f);
#endif
