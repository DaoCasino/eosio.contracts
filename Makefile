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

.PHONY: clean
clean:
	rm -rf build/*

.PHONY: tags
tags:
	ctags -R --exclude=build .
