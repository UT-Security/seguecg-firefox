.DEFAULT_GOAL := build

PARALLEL_COUNT=$(shell nproc)

bootstrap:
	cd .. && ./mach --no-interactive bootstrap --application-choice browser

# Some build dependency is broken, so run things twice
build-stock-debug:
	cd .. && MOZCONFIG=mozconfig_stock_debug ./mach build; MOZCONFIG=mozconfig_stock_debug ./mach build

build-segue-debug:
	cd .. && MOZCONFIG=mozconfig_segue_debug ./mach build; MOZCONFIG=mozconfig_segue_debug ./mach build

build-debug: build-stock-debug build-segue-debug

build-stock-release:
	cd .. && MOZCONFIG=mozconfig_stock_release ./mach build; MOZCONFIG=mozconfig_stock_release ./mach build

build-segue-release:
	cd .. && MOZCONFIG=mozconfig_segue_release ./mach build; MOZCONFIG=mozconfig_segue_release ./mach build

build: build-stock-release build-segue-release

clean:
	rm -rf ../ffbuilds