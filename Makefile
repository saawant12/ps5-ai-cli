IMAGE ?= ps5-ai-cli:rust1.95
SDK_IMAGE ?= ps5-ai-cli:sdk
ROOT := $(CURDIR)

.PHONY: toolchain probe process-probe loader-probe rust-loader-probe log-reader inspect test codex-check codex-build upload-probe

toolchain:
	docker build --build-arg "PS5_SDK_IMAGE=$(SDK_IMAGE)" -t $(IMAGE) .

probe:
	python3 tools/prepare-mio.py
	docker run --rm -v "$(ROOT):/work" $(IMAGE) tools/build-rust-probe.sh

process-probe:
	docker run --rm --network none -v "$(ROOT):/work" $(IMAGE) tools/build-process-probe.sh

loader-probe:
	python3 tools/fetch-loader.py
	docker run --rm --network none -v "$(ROOT):/work" $(IMAGE) tools/build-loader-probe.sh

rust-loader-probe:
	python3 tools/prepare-mio.py
	python3 tools/fetch-loader.py
	docker run --rm --network none -v "$(ROOT):/work" $(IMAGE) tools/build-loader-probe.sh rust

log-reader:
	docker run --rm --network none -v "$(ROOT):/work" $(IMAGE) -c '/opt/ps5-payload-sdk/bin/prospero-clang -O2 -Wall -Wextra -Werror probes/read-logs.c -o build/ps5-read-probe-logs.elf'

inspect:
	python3 tools/payload.py build/ps5-rust-runtime-probe.elf

test: inspect
	python3 -m unittest discover -s tools -p 'test_*.py'

codex-check:
	python3 tools/prepare-mio.py
	docker run --rm -v "$(ROOT):/work" $(IMAGE) tools/check-codex.sh

codex-build:
	python3 tools/prepare-mio.py
	docker run --rm -v "$(ROOT):/work" $(IMAGE) tools/check-codex.sh build

upload-probe: test
	@test -n "$(MANAGER)" || (echo 'Set MANAGER=http://PS5_IP:8084'; exit 1)
	python3 tools/payload.py build/ps5-rust-runtime-probe.elf --manager "$(MANAGER)" --run
