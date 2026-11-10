# padtool for Windows and Linux in $(O)/padtool-<build>.zip: padtool and
# padinstall, bleak for 64-bit Windows and x86-64 Linux, Python 3.11-3.14
# (fetched with uv), the RAM loaders and every board's firmware
# (tools/padtool/padtool_dist.py).
#   make padtool-dist
.PHONY: padtool-dist
padtool-dist: c2 n2 u2cw
	python3 tools/padtool/padtool_dist.py --build $(O) --version $(or $(BUILD_ID),dev) --out $(O)/padtool-$(or $(BUILD_ID),dev).zip
