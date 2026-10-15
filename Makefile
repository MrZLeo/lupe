.PHONY: install coverage test docs help
.DEFAULT_GOAL := help

define BROWSER_PYSCRIPT
import os, webbrowser, sys

try:
	from urllib import pathname2url
except:
	from urllib.request import pathname2url

webbrowser.open("file://" + pathname2url(os.path.abspath(sys.argv[1])))
endef
export BROWSER_PYSCRIPT

define PRINT_HELP_PYSCRIPT
import re, sys

for line in sys.stdin:
	match = re.match(r'^([a-zA-Z_-]+):.*?## (.*)$$', line)
	if match:
		target, help = match.groups()
		print("%-20s %s" % (target, help))
endef
export PRINT_HELP_PYSCRIPT

BROWSER := python3 -c "$$BROWSER_PYSCRIPT"
INSTALL_LOCATION := ~/.local
CPM_SOURCE_CACHE := $(CURDIR)/.cpm-cache
CMAKE_FLAGS := -DCMAKE_INSTALL_PREFIX=$(INSTALL_LOCATION) -DCPM_SOURCE_CACHE=$(CPM_SOURCE_CACHE)

help:
	@python3 -c "$$PRINT_HELP_PYSCRIPT" < $(MAKEFILE_LIST)

test: ## run tests quickly with ctest
	rm -rf build/
	cmake -Bbuild $(CMAKE_FLAGS) -DLupe_ENABLE_UNIT_TESTING=ON -DCMAKE_BUILD_TYPE="Release"
	cmake --build build --config Release
	cd build/ && ctest -C Release -VV

coverage: ## check code coverage quickly GCC
	rm -rf build/
	cmake -Bbuild $(CMAKE_FLAGS) -DLupe_ENABLE_UNIT_TESTING=ON -DLupe_ENABLE_CODE_COVERAGE=ON
	cmake --build build --config Release
	cd build/ && ctest -C Release -VV
	cd .. && (bash -c "find . -type f -name '*.gcno' -exec gcov -pb {} +" || true)

docs: ## generate Doxygen HTML documentation, including API docs
	rm -rf docs/
	rm -rf build/
	cmake -Bbuild $(CMAKE_FLAGS) -DLupe_ENABLE_UNIT_TESTING=OFF -DLupe_ENABLE_DOXYGEN=ON
	cmake --build build --config Release
	cmake --build build --target doxygen-docs
	$(BROWSER) docs/html/index.html

install: ## install the package to the `INSTALL_LOCATION`
	rm -rf build/
	cmake -Bbuild $(CMAKE_FLAGS) -DLupe_ENABLE_UNIT_TESTING=OFF
	cmake --build build --config Release
	cmake --build build --target install --config Release

format: ## format the project sources
	rm -rf build/
	cmake -Bbuild $(CMAKE_FLAGS) -DLupe_ENABLE_UNIT_TESTING=OFF
	cmake --build build --target clang-format
