# libjollymaam (no OS calls), built for this machine: linked into the
# jollymaam command, the host tests and the SDK.
TARGETS += host/libjollymaam.a
host/libjollymaam.a_type := archive
host/libjollymaam.a_toolchain := HOST_
host/libjollymaam.a_sources := $(wildcard $(self)/*.c $(self)/jieli_image/*.c)
host/libjollymaam.a_cflags := -std=c99
