/* jollymaam-pack: jollymaam's image commands without USB (no hidapi, no
 * device access), for firmware builds: jieli pack, 8bitdo pack and join,
 * with jollymaam's arguments. */
#include "jollymaam_8bitdo.h"
#include "jollymaam_jieli.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc >= 3 && !strcmp(argv[1], "jieli") && !strcmp(argv[2], "pack"))
    return jollymaam_jieli_app(argc - 1, argv + 1);
  if (argc >= 3 && !strcmp(argv[1], "8bitdo") && jollymaam_8bitdo_command(argv[2]))
    return jollymaam_8bitdo_app(argc - 1, argv + 1);
  fputs("usage: jollymaam-pack jieli pack ...\n"
        "       jollymaam-pack 8bitdo pack|join ...\n"
        "The image commands of jollymaam, with its arguments (jollymaam help jieli, help 8bitdo).\n",
        argc > 1 && strcmp(argv[1], "--help") ? stderr : stdout);
  return argc > 1 && strcmp(argv[1], "--help") ? 2 : 0;
}
