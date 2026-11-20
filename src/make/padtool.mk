# python-jollymaam: padtool for Windows and Linux in
# $(O)/python-jollymaam-<build>.zip (tools/padtool/padtool_dist.py): padtool
# and padinstall, the firmware package, jollymaam (Linux; Windows too with
# JOLLYMAAM_WINDOWS=path/to/jollymaam.exe), bleak for 64-bit Windows and
# x86-64 Linux, Python 3.11-3.14 (fetched with uv).  CI attaches it to a
# release as python-jollymaam.zip.
#   make padtool-dist
.PHONY: padtool-dist
padtool-dist: firmware
	python3 tools/padtool/padtool_dist.py --package $(FIRMWARE_ZIP) --jollymaam $(JOLLYMAAM_CLI) \
		$(if $(JOLLYMAAM_WINDOWS),--jollymaam-windows $(JOLLYMAAM_WINDOWS)) \
		--version $(or $(BUILD_ID),dev) --out $(O)/python-jollymaam-$(or $(BUILD_ID),dev).zip
