#ifndef JOLLYMAAM_8BITDO_APP_H
#define JOLLYMAAM_8BITDO_APP_H
#include <stdio.h>
int jollymaam_8bitdo_command(const char *command);
int jollymaam_8bitdo_app(int argc, char **argv);
void jollymaam_8bitdo_usage(FILE *out);
void jollymaam_8bitdo_scan(void);
#endif
