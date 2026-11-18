# The web page (web/README.md): its tests, in node, one with packages the
# jollymaam command makes.
WEB_TESTS := $(wildcard $(self)/test/*.test.mjs)
WEB_VECTORS := $(self)/test/python-vectors.json

TRACKED += $(O)/web/test.ok
$(O)/web/test.ok: $(wildcard $(self)/test/*.mjs) $(WEB_VECTORS) $(wildcard $(self)/*.js) $(JOLLYMAAM_CLI) | $(O)/web/
	$(call lib-run,JOLLYMAAM=$(JOLLYMAAM_CLI) node --test $(WEB_TESTS) > $(@D)/test.log && touch $@,TEST)

.PHONY: web-test
web-test: $(O)/web/test.ok
test: web-test

# Catalog checks include every setting, not just a connected board's fields.
$(O)/web/test.ok: $(wildcard $(self)/strings/*.json) $(self)/i18n.js tools/strings.py tools/settings_strings.c src/core/settings.c src/core/pad.h

.PHONY: strings-check strings-report strings-sync
strings-check:
	python3 tools/strings.py check --cc '$(HOST_CC)'
strings-report:
	python3 tools/strings.py report --output $(O)/strings.html
strings-sync:
	python3 tools/strings.py sync-settings --cc '$(HOST_CC)'
web-test: strings-check

.PHONY: mock-fixture
mock-fixture:
	python3 tools/strings.py mock-fixture --cc '$(HOST_CC)'
$(O)/web/test.ok: $(self)/mock-fields.json
