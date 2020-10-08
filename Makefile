contracts_ver := $(shell git describe --tags --dirty)

default: help

.PHONY: help
help:
	@echo "Targets:"
	@echo
	@echo "  build            : build contracts & tests"
	@echo
	@echo "  run-unit-tests   : run all unit tests"
	@echo "  run-verbose-test : run particular test with maximal verbosity;"
	@echo "                     e.g.: make t=test1 run-verbose-test"
	@echo
	@echo "  docs             : generate doxygen documentation"
	@echo "  clean            : clean build/"
	@echo "  tags             : generate tags file via ctags"

.PHONY: build
build:
	./cicd/build.sh --build-type Debug --build-tests

.PHONY: run-unit-tests
run-unit-tests:
	./build/tests/unit_tests -l message -p

.PHONY: run-verbose-test
run-verbose-test:
	./build/tests/unit_tests -l all -r detailed -t tests/"$(t)" -- --verbose

# see also `cicd/build.sh docs'
.PHONY: docs
docs:
	mkdir -p build/docs/doxygen
	env \
		DOXY_CONTRACTS_VERSION="$(contracts_ver)" \
		DOXY_DOC_DEST_DIR=build/docs/doxygen \
		DOXY_DOC_INPUT_DIRS="contracts/ tests/" \
		DOXY_HAVE_DOT=YES \
		envsubst '$$DOXY_CONTRACTS_VERSION $$DOXY_DOC_DEST_DIR $$DOXY_DOC_INPUT_DIRS $$DOXY_HAVE_DOT' \
		< doxyfile.in > build/doxyfile
	doxygen build/doxyfile

.PHONY: clean
clean:
	rm -rf build/*

.PHONY: tags
tags:
	ctags -R --exclude=build .
