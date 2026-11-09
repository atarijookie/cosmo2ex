# global makefile - thin wrapper around the per-directory makefiles and build scripts,
# the same steps as .github/workflows/ci.yml


.PHONY: default test atari firmware android docker release clean

default:
	@echo "usage :  make <test|atari|firmware|android|docker|release|clean>"
	@echo "  test     - libdospath + ce_cores + Pico firmware (host) unit tests, webserver vet + tests"
	@echo "  atari    - drivers (ce_dd, ce_eth.stx, ce_eth.xif, ce_sting.prg, ce_brows.prg, ce_video.ttp, ce_audio.ttp, ce_doom.prg), boot sectors and ce_tests.tos (needs m68k-atari-mint-gcc)"
	@echo "  firmware - Pico firmwares (.uf2) into firmware/out/, see firmware/build.sh (needs docker)"
	@echo "  android  - Android floppy app into webserver/static/app/, see android/build.sh (needs docker)"
	@echo "  docker   - docker images for all platforms with the Android app, see docker/build_all.sh"
	@echo "  release  - release tarballs with the Android app, see release/build.sh"

test:
	$(MAKE) -C libdospath test
	$(MAKE) -C ce_cores test
	$(MAKE) -C ce_pico_tests test
	cd webserver && go vet ./... && go test ./...
	cd ce_media && go vet ./... && go test ./...
	cd ce_floppy_gen && go vet ./... && go test ./...

atari:
	docker/build_drivers.sh
	$(MAKE) -C test_ce

firmware:
	firmware/build.sh

android:
	android/build.sh

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
	$(MAKE) -C ce_sting_stx_driver clean
	$(MAKE) -C ce_mintnet_xif_driver clean
	$(MAKE) -C ce_fakesting clean
	$(MAKE) -C ce_browser_prg clean
	$(MAKE) -C ce_media_prg clean
	$(MAKE) -C third_party/doomgeneric clean
	$(MAKE) -C test_ce clean
	rm -rf firmware/out
	rm -rf webserver/static/app android/.gradle-cache
