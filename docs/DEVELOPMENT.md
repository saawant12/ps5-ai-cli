# Building and testing PS5 AI CLI

For setup, controls and current availability, see the [README](../README.md).
This guide covers the native port, development builds and diagnostics.

## Build

The tested host is ARM64 Linux through Docker. Install Docker, Git, Python 3,
and Node.js/npm on the host. Clone this repository and run the following
commands from its root to build the SDK and Rust toolchain, then fetch the
pinned Codex source:

```sh
make sdk
make toolchain
python3 tools/fetch-codex.py
make terminal
```

The SDK bootstrap verifies the archive hashes in `sources.lock.json`, installs
LLVM 19 and cross-compiles OpenSSL/curl. No sibling repository or private Docker
image is required. Downloads require internet access on the build machine;
the console can remain offline for startup and terminal testing.

Use `make terminal-release` for the optimized ELF. For development diagnostics,
run `make probe`, `make test`, `make codex-check`, or `make codex-build`.

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

## Terminal development build

`make terminal` embeds the offline xterm.js assets, certificate roots, PS5
shortcut, native shell and file tools, then builds `build/ps5-ai-cli.elf` through
a locked Cargo invocation. The pinned shell/loader sources are fetched by Make;
the native build checks and patches those sources without downloading them.
`make terminal-release` selects the optimized profile. Node.js/npm is required
to install the pinned web packages. Optimized development builds have reached
native onboarding; complete beta behavior has not yet passed hardware tests.

`make terminal-dev` is an optional faster relink from the last captured Rust
build. The normal `make terminal` path does not depend on those captured objects.

The app uses title `PAIC00001`, HTTP port `8035`, and storage under
`/data/ps5-ai-cli`. The shortcut installer preserves unrelated applications and
refuses conflicting title files. It binds the gateway before installation so a
second launch cannot change shortcut files while another instance is running.

The payload locates its installed ELF by an embedded build marker in Payload
Manager and copies it into its private runtime directory. Both grouped and
per-upload Payload Manager directories are supported. It installs a versioned
Dash shell and 28 sbase tools for native command execution; existing unrelated
or modified runtime files are not silently overwritten.

The raw terminal bridge carries Codex input/output and window-size changes. It
is not a kernel PTY and does not implement cooked terminal input or shell job
control. Native child commands use the explicit PS5 SDK loader and pipes.
Ordinary `std::process::Command` calls and arbitrary Linux/FreeBSD binaries are
not generally supported. Interactive child programs requiring a kernel PTY
remain unsupported.

Codex uses file-based credential storage. PS5 keyring requests return
Unsupported; upstream file and ephemeral stores remain available. PATH aliases
are also unsupported. The standalone app-server diagnostic uses capability-token
authentication; missing or incorrect tokens return HTTP 401, and browser Origin
requests return 403. This diagnostic is separate from the terminal gateway.

Browsers exposing the standard Gamepad API have mappings for X/Enter,
Circle/Escape, Square/text entry, Triangle/Tab and Options/toolbar focus. These
mappings pass browser simulations but do not establish physical PS5 support.
The README lists the controls actually verified on the console.

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

## Verified compatibility

| Component | Hardware result |
| --- | --- |
| Firmware and loader | PS5 13.60, Relapse / elfldr, Payload Manager v0.5.2 |
| Rust 1.95 runtime | Allocation, file I/O, threads, mutexes, thread sleep, Tokio timers and async TCP passed |
| Codex 0.160.0 | Native version output and authenticated app-server initialization, account status and thread listing passed |
| Interactive terminal | Native Codex device-code login, login persistence across a payload restart, a model reply, physical X confirmation, and D-pad/X keypad entry passed |
| Native child loader | Rust FFI launch with an active Tokio runtime passed arguments, environment, working directory, standard streams, exit status and cancellation checks |
| Native command runtime | Codex command/pipe interfaces, large output, file-edit checks, shell pipelines, scripts, error statuses, and repeated tool workflows passed in isolated diagnostics |
| Bundled shell and tools | Native first and repeat installation of the pinned Dash shell and 28 sbase tools passed |
| HTTPS diagnostic | Native DNS and curl/OpenSSL verification of the login host passed with bundled CA roots; self-signed and wrong-host certificates were rejected |

These diagnostics do not establish general `std::process::Command` compatibility
or a complete interactive coding workflow. Versions and upstream hashes are recorded in
[sources.lock.json](../sources.lock.json).

## Validation

`make test` runs local ELF-validation, payload-cleanup and launcher-isolation tests after checking
the built Rust probe. The system-kernel ELF case also requires a built loader
probe; it is skipped when that optional artifact is absent.

`make terminal-test` builds an isolated local PTY host and checks the browser
gateway's authentication, origin checks and terminal transport. It requires a C
compiler on macOS, or a C compiler and OpenSSL development headers on Linux.
These checks run local shell commands; they do not run commands on the console.

`Dockerfile.validation` and `tools/validate-upstream.sh` support selected upstream
checks. Across the affected host test runs, 340 tests passed and nine were
skipped. Formatting and Bazel lock regeneration also passed. These results do not
represent a full upstream test-suite run or a complete PS5 coding task.
The native command changes additionally passed the focused process suite
(62 tests) and shell-command suite (192 tests). Bundle installation and helper
entry checks run with `python3 -m unittest tools/test_runtime_bundle.py` in the
Linux validation container after building and embedding the runtime assets.
