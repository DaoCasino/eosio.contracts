default: help

.PHONY: help
help:
	@echo "Targets:"
	@echo
	@echo "  tags : generate tags file via ctags"


.PHONY: tags
tags:
	ctags -R --exclude=build .
