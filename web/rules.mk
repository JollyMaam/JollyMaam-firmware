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
