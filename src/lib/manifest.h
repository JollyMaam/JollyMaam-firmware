#ifndef JM_MANIFEST_H
#define JM_MANIFEST_H
#include "package.h"
struct jm_firmware_target {
  char product[17], board[64], chip[16];
  struct jm_package_entry install, update, loader;
  uint32_t preserve_offset, preserve_size, model;
};
struct jm_manifest { unsigned count; struct jm_firmware_target targets[8]; };
int jm_manifest_open(struct jm_manifest *out, const struct jm_package *package);
const struct jm_firmware_target *jm_manifest_target(const struct jm_manifest *manifest, const char *product);
#endif
