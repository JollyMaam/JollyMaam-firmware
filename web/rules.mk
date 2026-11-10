# The web page (web/README.md): its tests, in node, one with packages the
# jollymaam command makes.
WEB_TESTS := $(wildcard $(self)/test/*.test.mjs)

TRACKED += $(O)/web/test.ok
$(O)/web/test.ok: $(WEB_TESTS) $(wildcard $(self)/*.js) $(JOLLYMAAM_CLI) | $(O)/web/
	$(call lib-run,JOLLYMAAM=$(JOLLYMAAM_CLI) node --test $(WEB_TESTS) > $(@D)/test.log && touch $@,TEST)

.PHONY: web-test
web-test: $(O)/web/test.ok
test: web-test
