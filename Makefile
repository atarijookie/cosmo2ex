# global makefile - thin wrapper around the per-directory makefiles and build scripts,
# the same steps as .github/workflows/ci.yml


.PHONY: default test atari firmware docker release clean

default:
	@echo "usage :  make <test|atari|firmware|docker|release|clean>"
	@echo "  test     - libdospath + ce_cores + Pico firmware (host) unit tests, webserver vet + tests"
	@echo "  atari    - drivers, boot sectors and ce_tests.tos (needs m68k-atari-mint-gcc)"
	@echo "  firmware - Pico firmwares (.uf2) into firmware/out/, see firmware/build.sh (needs docker)"
	@echo "  docker   - docker images for all platforms, see docker/build_all.sh"
	@echo "  release  - release tarballs, see release/build.sh"

test:
	$(MAKE) -C libdospath test
	$(MAKE) -C ce_cores test
	$(MAKE) -C ce_pico_tests test
	cd webserver && go vet ./... && go test ./...

atari:
	docker/build_drivers.sh
	$(MAKE) -C test_ce

firmware:
	firmware/build.sh

docker:
	docker/build_all.sh

release:
	release/build.sh

clean:
	$(MAKE) -C libdospath clean
	$(MAKE) -C ce_cores clean
	$(MAKE) -C ce_pico_tests clean
	$(MAKE) -C libacsiscsi clean
	$(MAKE) -C ce_dd_prg clean
	$(MAKE) -C ce_dd_bootsectors clean
	$(MAKE) -C test_ce clean
	rm -rf firmware/out
