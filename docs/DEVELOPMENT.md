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
to install the pinned web packages. Native sign-in, replies, CLI restart, and
a bounded model-driven shell coding task have passed on the tested console.
See the README for the remaining compatibility limits.

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

The PS5 model adapter selects Codex's direct tools for bundled, cached and remote
model metadata. The V8-based `codex-code-mode-host` is not bundled for this target.
Direct tools retain upstream execution-policy, approval and environment handling.

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

The gateway owns one CLI child in a separate process, so a CLI exit leaves the
picker and restart endpoint available. A private datagram descriptor carries
window sizes to the child's raw terminal adapter. Restart closes the attached
relay, stops and reaps the owned child with bounded waits, and launches it again;
it does not wipe credentials or workspace files. It does not automatically
relaunch a CLI after an exit.

`POST /api/cli/codex/restart` requires a paired browser session, matching Origin,
client header and empty body. Other CLI names are unavailable. The host gateway
suite covers a hung process restart, retained pairing and saved files. Native
supervisor tests cover forced termination, reaping, explicit relaunch and an
unrelated process surviving a restart. On the tested PS5, restart reaped the
old Codex child and opened a new signed-in CLI while the gateway and unrelated
payloads stayed running. Gateway workers use an explicit stack size for native
runtime installation and loading. The SDK ELF validator checks the complete
CLI image as well as the bundled tools, including the SDK's supported absolute
64-bit symbol relocations.

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
| Model-driven coding smoke test | Native Codex created a shell script, ran it, edited it, and verified exact outputs for explicit and default arguments |
| HTTPS diagnostic | Native DNS and curl/OpenSSL verification of the login host passed with bundled CA roots; self-signed and wrong-host certificates were rejected |

These results do not establish general `std::process::Command` compatibility
or support for broader project toolchains. Versions and upstream hashes are recorded in
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
represent a full upstream test-suite run. Native results are listed separately
in the compatibility table above.
The native command changes additionally passed the focused process suite
(62 tests) and shell-command suite (192 tests). Bundle installation and helper
entry checks run with `python3 -m unittest tools/test_runtime_bundle.py` in the
Linux validation container after building and embedding the runtime assets.
The model-selection adapter passed the scoped models-manager suite (55 tests).
The command proxy's signal ownership, failed-launch restoration, and exit/signal
propagation are covered by `python3 -m unittest tools.test_shell_exec`.

The terminal adapter's host tests require macOS or FreeBSD because they exercise
the BSD ioctl ABI. Runtime image/tool installation tests run on Linux. The
installation suites use disposable trees to check first install, repeated
install, owned runtime replacement in either version direction, deleted-shortcut
recovery, and preservation of saved data and unrelated files.

## Release packaging

Release payloads must come from `make terminal-release` with development pairing
disabled (the default). The build forces command tracing off. A release build
uses a fresh six-digit pairing code each time the gateway starts. The code
accepts remote pairing for 15 minutes; paired browser sessions last eight hours.
The console-only Pair device panel displays and renews the code without restarting
the gateway or CLI. Its endpoint requires a paired session, matching Origin,
client header, empty POST body and a loopback peer; remote paired browsers
cannot retrieve or renew it.
The PS5's local shortcut pairs through the loopback-only endpoint.

Keep build logs, device details and test workspaces out of published archives.
After reviewing the corresponding-source and notices archives, package the ELF:

```sh
python3 tools/package-release.py --build-root /path/to/clean-build \
  --source /path/to/reviewed-source.tar.gz \
  --notices /path/to/reviewed-notices.tar.gz \
  --revision FULL_SOURCE_COMMIT_SHA --output build/release
```

The packager recomputes the production build ID and checks the ELF ownership
note before copying any assets. A development pairing build or captured-object
relink has a different ID and is rejected. The output contains one installable
`ps5-ai-cli.elf`, matching source and notices archives, a build manifest, and
`SHA256SUMS`. This check does not substitute for running the candidate on PS5.
