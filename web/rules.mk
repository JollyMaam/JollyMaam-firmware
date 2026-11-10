# The web page (web/README.md): its protocol tests, in node.
WEB_TESTS := $(wildcard $(self)/test/*.test.mjs)

TRACKED += $(O)/web/test.ok
$(O)/web/test.ok: $(WEB_TESTS) $(wildcard $(self)/*.js) | $(O)/web/
	$(call lib-run,node --test $(WEB_TESTS) > $(@D)/test.log && touch $@,TEST)

.PHONY: web-test
web-test: $(O)/web/test.ok
test: web-test
