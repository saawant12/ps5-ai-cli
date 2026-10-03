# ps5-ai-cli

Experimental native port of [OpenAI Codex](https://github.com/openai/codex) for
jailbroken PlayStation 5 consoles.

**Development source only. There is no usable beta or ready-to-install app yet.**
Account login, model replies and complete coding tasks have not been verified.
This repository does not currently provide an interactive PS5 or browser interface.

## Verified compatibility

| Component | Hardware result |
| --- | --- |
| Firmware and loader | PS5 13.60, Relapse / elfldr, Payload Manager v0.5.2 |
| Rust 1.95 runtime | Allocation, file I/O, threads, mutexes, Tokio timers and async TCP passed |
| Codex 0.160.0 | Native version output and authenticated app-server initialization, account status and thread listing passed |
| Native child loader | Rust FFI launch with an active Tokio runtime passed arguments, environment, working directory, standard streams, exit status and cancellation checks |
| HTTPS diagnostic | Native curl/OpenSSL verified the login host and rejected self-signed and wrong-host certificates using supplied CA roots and temporary host-resolved addresses; native DNS failed |

The child-loader result does not establish `std::process::Command` compatibility
or working Codex shell tools. Versions and upstream hashes are recorded in
[sources.lock.json](sources.lock.json).

## Build

The current build requires a locally supplied ARM64 Linux Docker image containing
ps5-payload-sdk 0.43 at `/opt/ps5-payload-sdk`, LLVM 19, Python 3 and PS5 OpenSSL/curl
libraries under the SDK's `target/user/homebrew` directory. The default base-image
tag, `ps5-ai-cli:sdk`, is a local prerequisite, not a published Docker Hub image.
A standalone SDK bootstrap is not included. Git and Python 3 are also required on
the host.

```sh
make toolchain SDK_IMAGE=your-sdk-image:tag
python3 tools/fetch-codex.py
make probe
make test
make codex-check
make codex-build
```

Full Codex builds default to one compilation job. Avoid concurrent large builds
in memory-constrained Docker environments.

- `make probe` builds `build/ps5-rust-runtime-probe.elf`.
- `make codex-check` checks compilation; it does not link or run an ELF.
- `make codex-build` builds a diagnostic ELF invoking the real CLI with
  `--version`. It does not start an interactive session.
- `make process-probe`, `make loader-probe` and `make rust-loader-probe` build
  development diagnostics for native process behavior.
- `make log-reader` builds the temporary diagnostic log collector.
- `make network-probe` builds a bounded DNS/HTTPS diagnostic. Supply a CA PEM
  bundle at `build/network-probe-ca.pem` first; the build does not download roots.

The custom target rebuilds Rust `std` for the FreeBSD 11 libc ABI, uses
position-independent code and disables native ELF TLS. The SDK provides process
startup and native `.sprx` imports. ABI adapters are in `platform/`; upstream
changes are in `patches/`. Codex uses the resolution lock in `locks/codex/`.

The tested app-server ELF was linked from captured Rust objects using
`tools/relink-codex.py --service` and locally generated configuration. That helper
is not a standalone reproducible service build.

## Console diagnostics

Payload Manager's HTTP interface can upload and launch a development payload
through the console's local loader; an externally listening port 9021 is not
required. This route was tested with Payload Manager v0.5.2.

```sh
make upload-probe MANAGER=http://PS5_IP:8084
```

The uploader validates the ELF, uses a content-derived filename, records its own
uploads locally, and removes recorded superseded builds of the same payload.
It refuses to relaunch an identical installed copy automatically and never
retries a launch. Unrecorded payloads are not selected for deletion. Payload
Manager can remove autoload entries associated with deleted payloads. An HTTP
`OK` response confirms handoff to the loader, not successful execution.

The Rust probe reports notifications and writes its result under
`/data/ps5-ai-cli/`. It makes no AI requests and requires no API credentials.

The optional log collector serves `http://PS5_IP:19061/logs` once, then exits.
It reads only the project's matching runtime and service logs: the newest 16
files, at most 64 KiB per file. It times out after 90 seconds without a connection
and does not install an autostart service.

## Limitations

- Native PTY creation returns `ENOSYS`; a shell is not bundled.
- Ordinary kernel execution rejects the tested SDK ELF. The separate child
  loader has not been integrated into Codex command execution.
- Codex device login currently fails at hostname resolution. The native resolver
  fails in the tested environment, and the expected system CA paths are absent.
  Isolated curl/OpenSSL HTTPS checks passed with supplied roots and addresses;
  this does not verify Codex login, token exchange or model requests.
- The app-server uses capability-token authentication. Missing or incorrect
  tokens return HTTP 401; browser Origin requests return 403. A browser gateway
  and PS5 interface are not included.
- PS5 keyring requests return Unsupported; upstream file and ephemeral auth
  stores remain available. PATH aliases are also unsupported.

## Validation

`make test` runs ten local ELF-validation and payload-cleanup tests after checking
the built Rust probe. The system-kernel ELF case also requires a built loader
probe; it is skipped when that optional artifact is absent.

`Dockerfile.validation` and `tools/validate-upstream.sh` support selected upstream
checks. Across the affected host test runs, 340 tests passed and nine were
skipped. Formatting and Bazel lock regeneration also passed. These results do not
represent a full upstream test-suite run or a complete PS5 coding task.

## License

Original project code is licensed under **GPL-3.0-or-later**: GNU General Public
License version 3, or (at your option) any later version. See [LICENSE](LICENSE).
Third-party code retains its respective licenses; see
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and [LICENSES](LICENSES).

Upstream projects: [Codex](https://github.com/openai/codex),
[PS5 SDK](https://github.com/ps5-payload-dev/sdk),
[shsrv](https://github.com/ps5-payload-dev/shsrv),
[Payload Manager](https://github.com/itsPLK/ps5-payload-manager),
[Relapse](https://github.com/ntfargo/Relapse-Exploit).
