#include "../../src/lib/transport_hid.h"
#include "../../src/lib/transport_custom.h"
#include "../../src/lib/package.h"
#include "../../src/lib/manifest.h"
#include "../../src/lib/proto_jollymaam.h"
#include "../../src/lib/jieli_image/ufw.h"
#include "../../src/lib/jieli_image/entry.h"
#include "../../src/lib/jieli_image/image.h"
#include "../../src/lib/transport_jieli.h"
#include "jollymaam_update.h"
#include "jollymaam_jieli.h"
#include "jollymaam_restore.h"
#include "jollymaam_8bitdo.h"
#include "jollymaam_dev.h"
#include "../../src/lib/platform.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_image(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  long n;
  if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 32 * 1024 * 1024 ||
      fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
  uint8_t *data = malloc(n ? (size_t)n : 1);
  if (!data) { fclose(f); return NULL; }
  if (fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return NULL; }
  fclose(f); *size = (size_t)n; return data;
}
static int rekey(const char *input, const char *output, const char *text) {
  char *end;
  errno = 0;
  unsigned long key = strtoul(text, &end, 0);
  if (errno || !*text || *end || key > 0xffff) {
    fprintf(stderr, "Key must be a 16-bit integer\n"); return 1;
  }
  size_t size;
  uint8_t *data = read_image(input, &size);
  if (!data) { fprintf(stderr, "Cannot read input image\n"); return 1; }
  uint8_t *workspace = malloc(size ? size : 1), *packed = malloc(size ? size : 1);
  struct jm_jieli_image view;
  int result = 1;
  if (!workspace || !packed) goto done;
  int error = jm_jieli_image_open(&view, data, size, workspace, size);
  if (!error) error = jm_jieli_image_pack(&view, (uint16_t)key, packed, size);
  if (error) { fprintf(stderr, "Invalid Jieli image: %d\n", error); goto done; }
  FILE *file = fopen(output, "wb");
  if (!file) { perror(output); goto done; }
  int written = fwrite(packed, 1, size, file) == size;
  int closed = fclose(file) == 0;
  if (!written || !closed) { fprintf(stderr, "Cannot write output image\n"); goto done; }
  result = 0;
done:
  free(data); free(workspace); free(packed);
  return result;
}
/* Help is resolved before any image/device operation, including --help after
 * a command. Groups retain argv[0] as their name, so subcommand argument offsets
 * are shared with the existing command frontends. */
static int help_word(const char *word) { return !strcmp(word,"help") || !strcmp(word,"--help"); }
static void usage(FILE *out) {
  fprintf(out,"usage: jollymaam <command>\n"
              "  scan [--ours]            What is connected, by USB port (--ours: \"PORT PID\" of our firmware)\n"
              "  update [--device PATH] [--product PRODUCT] [--yes] FIRMWARE.zip\n"
              "                           Our firmware through its updater\n"
              "  inspect IMAGE            Inspect a package or firmware image\n"
              "  jieli <subcommand>       Jieli (GameSir) images and ROM loader\n"
              "    pack, list, verify, unpack, rekey, flash, flashpack, backup, loader, run,\n"
              "    install, check-stock, restore\n"
              "  8bitdo <subcommand>      8BitDo images and boot loader (Telink and Yichip pads)\n"
              "    pack, join, flash, loader, start, check-stock, restore\n"
              "  help [jieli|8bitdo]      Show help (--help also works)\n"
              "jollymaam-pack runs the image commands (jieli pack, 8bitdo pack and join) without USB.\n");
}
static int group_help(int family) {
  if(family==1) {
    char *args[]={"jieli","--help"};return jollymaam_jieli_app(2,args);
  }
  jollymaam_8bitdo_usage(stdout);
  return 0;
}
/* Everything connected, by USB port; --ours: "PORT PID" per device running
 * our firmware (make flash-all reads it). */
static int scan(int ours) {
  static struct hid_dev h[64];
  if(ours) {
    size_t n=hid_devices(h,64);char pid[17],port[64];
    for(size_t i=0;i<n;++i)
      if(is_tool(&h[i]) && tool_pid(&h[i],pid)) printf("%s %s\n",usb_port(h[i].path,port)?port:h[i].path,pid);
    return 0;
  }
  jollymaam_jieli_scan();
  jollymaam_8bitdo_scan();
  return 0;
}
int main(int argc, char **argv) {
  int family=0;
  if(argc==1) { usage(stdout);return 0; }
  if(help_word(argv[1])) {
    if(argc==2) { usage(stdout);return 0; }
    if(argc==3 && !strcmp(argv[2],"jieli")) return group_help(1);
    if(argc==3 && !strcmp(argv[2],"8bitdo")) return group_help(2);
    fprintf(stderr,"Unknown help topic\n");usage(stderr);return 2;
  }
  if(!strcmp(argv[1],"jieli")) family=1;
  else if(!strcmp(argv[1],"8bitdo")) family=2;
  if(family) { --argc;++argv; }
  if(family && argc==1) return group_help(family);
  for(int i=1;i<argc;++i) if((i==1 && help_word(argv[i])) || !strcmp(argv[i],"--help")) {
    if(family) return group_help(family);
    usage(stdout);return 0;
  }
  if(family==1 && jollymaam_jieli_command(argv[1])) return jollymaam_jieli_app(argc,argv);
  if(family==2 && jollymaam_8bitdo_command(argv[1])) return jollymaam_8bitdo_app(argc,argv);
  if(family && (!strcmp(argv[1],"restore") || !strcmp(argv[1],"check-stock")))
    return jollymaam_restore_app(argc,argv,family==1);
  if (!family && !strcmp(argv[1], "update")) return jollymaam_update_command(argc-2,argv+2);
  if (family==1 && argc == 6 && !strcmp(argv[1], "install") && !strcmp(argv[2], "--yes")) {
    size_t loader_size = 0, image_size = 0;
    uint8_t *loader = read_image(argv[4], &loader_size), *image = read_image(argv[5], &image_size);
    int result = loader && image ? jm_jieli_upgrade(argv[3], loader, loader_size, image, image_size) : -1;
    free(loader); free(image);
    if (result) fprintf(stderr, "Jieli upgrade failed: %d; recovery may be required\n", result);
    else puts("Written and verified; device remains in its loader");
    return result ? 1 : 0;
  }
  if (family==1 && argc == 5 && !strcmp(argv[1], "rekey"))
    return rekey(argv[2], argv[3], argv[4]);

  if (!family && !strcmp(argv[1], "scan") && (argc == 2 || (argc == 3 && !strcmp(argv[2], "--ours"))))
    return scan(argc == 3);
  if (!family && argc == 3 && !strcmp(argv[1], "inspect")) {
    size_t size;
    uint8_t *data = read_image(argv[argc - 1], &size);
    if (!data) { fprintf(stderr, "Cannot read image (limit 32 MiB)\n"); return 1; }
    int result = 1;
    {
      char product[17];
      const uint8_t *flash;
      size_t flash_size;
      struct jm_jieli_image view;
      uint8_t *workspace = malloc(size ? size : 1);
      enum jm_jieli_ufw_result ufw = jm_jieli_ufw_open(data, size, &flash, &flash_size);
      struct jm_package package;
      if (size >= 4 && !memcmp(data, "PK\003\004", 4)) {
        int error = jm_package_open(&package, data, size);
        if (error) fprintf(stderr, "Invalid/unsupported ZIP container: %d\n", error);
        else {
          struct jm_manifest manifest;
          int verified = jm_manifest_open(&manifest, &package);
          if (!verified) fprintf(stderr, "Manifest validation failed\n");
          else printf("Verified firmware manifest: %u targets\n", manifest.count);
          printf("Validated ZIP container: %u entries\n", package.count);
          for (unsigned i = 0; i < package.count; ++i) {
            struct jm_package_entry entry;
            if (jm_package_entry(&package, i, &entry))
              printf("%.*s: %zu bytes\n", (int)entry.name_size, entry.name, entry.size);
          }
          result = verified ? 0 : 1;
        }
      } else if (jm_update_image_check(data, size, NULL)) {
        printf("Validated MAIN update image: bytes=%zu\n", size); result = 0;
      } else if (jm_8bitdo_image_product(data, size, product)) {
        printf("Telink full image: product=%s bytes=%zu\n", product, size); result = 0;
      } else if ((ufw == JM_JIELI_UFW_RAW || ufw == JM_JIELI_UFW_OK) &&
                 workspace && jm_jieli_image_open(&view, flash, flash_size, workspace, size) == JM_JIELI_IMAGE_OK) {
        printf("Jieli image: base=0x%x bytes=%zu wrapper=%s key=0x%04x (validated)\n",
               view.base, flash_size, ufw == JM_JIELI_UFW_OK ? "UFW" : "raw", view.key); result = 0;
      } else fprintf(stderr, "Unrecognized or invalid image\n");
      free(workspace);
    }
    free(data); return result ? 1 : 0;
  }
  if(family) {
    fprintf(stderr,"Unknown or incomplete %s subcommand: %s\n",family==1?"jieli":"8bitdo",argv[1]);
    group_help(family);
  } else { fprintf(stderr,"Unknown or incomplete command: %s\n",argv[1]);usage(stderr); }
  return 2;
}
