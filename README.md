# ps5-ai-cli

An experimental home for AI coding CLIs on jailbroken PlayStation 5 consoles,
with a simple CLI picker and terminal access from the PS5, a computer or a phone.
Choose an available CLI to open its own interface. The launcher does not manage
conversations or replace the CLI's prompts, login or approvals.

[OpenAI Codex](https://github.com/openai/codex) is the first native port and the
focus of the initial beta. The project is intended to expand to **Claude Code,
Antigravity, Devin, and other AI CLIs** in later versions. Those integrations
are not available yet; this is a multi-CLI project with a Codex-first beta.

**Development source only. There is no usable beta or ready-to-install app yet.**
Account login, model replies and complete coding tasks have not been verified.
A terminal interface is under development. Desktop and phone-sized browser tests
have exercised the actual Codex CLI through a host PTY. The same terminal bridge
has also reached Codex's native onboarding screen on PS5. Account login and
coding-tool execution remain unverified.

## Verified compatibility

| Component | Hardware result |
| --- | --- |
| Firmware and loader | PS5 13.60, Relapse / elfldr, Payload Manager v0.5.2 |
| Rust 1.95 runtime | Allocation, file I/O, threads, mutexes, Tokio timers and async TCP passed |
| Codex 0.160.0 | Native version output and authenticated app-server initialization, account status and thread listing passed |
| Interactive terminal | Native Codex onboarding rendered through the PS5-hosted browser terminal; login and coding tools remain unverified |
| Native child loader | Rust FFI launch with an active Tokio runtime passed arguments, environment, working directory, standard streams, exit status and cancellation checks |
| HTTPS diagnostic | Native curl/OpenSSL verified the login host and rejected self-signed and wrong-host certificates using supplied CA roots and temporary host-resolved addresses; native DNS failed |

The child-loader result does not establish `std::process::Command` compatibility
or working Codex shell tools. Versions and upstream hashes are recorded in
[sources.lock.json](sources.lock.json).

## Build

The tested host is ARM64 Linux through Docker. Install Docker, Git, Python 3,
and Node.js/npm on the host. Build the SDK and Rust toolchain from their public
sources, then fetch the pinned Codex source:

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

`make terminal` embeds the offline xterm.js assets, certificate roots and PS5
shortcut, then builds `build/ps5-ai-cli.elf` through a locked Cargo invocation.
`make terminal-release` selects the optimized profile. Node.js/npm is required
to install the pinned web packages. Optimized development builds have reached
native onboarding; complete beta behavior has not yet passed hardware tests.

`make terminal-dev` is an optional faster relink from the last captured Rust
build. The normal `make terminal` path does not depend on those captured objects.

The application uses title `PAIC00001`, HTTP port `8035`, and storage under
`/data/ps5-ai-cli`. Its home-screen shortcut opens the local CLI picker. The
installer preserves unrelated applications and refuses conflicting title files.
The shortcut opens an already-running payload; reboot/autostart behavior is
not yet verified. The shortcut pairs the console locally over loopback. Remote
browsers pair using the code in the PS5 notification.

The raw terminal bridge carries Codex input/output and window-size changes. It
is not a general-purpose kernel PTY and does not implement cooked terminal
input, shell job control, or the native command backend. Only Codex is selectable;
other CLI ports are unavailable. Login and coding behavior belong to Codex.
The terminal embeds JetBrains Mono for consistent offline text rendering.
On the tested PS5 browser, the D-pad changes CLI selections and the on-screen
**Enter** button works. **Physical X does not send Enter directly to the terminal**;
activate the on-screen Enter button instead. This is a known beta limitation.

Browsers exposing the standard Gamepad API also have mappings for X/Enter,
Circle/Escape, Square/text entry, Triangle/Tab, and Options/toolbar focus. Face
buttons act once per press. These mappings pass browser simulations, but that
does not establish support in the PS5 browser. Normal keyboard typing and
shortcuts pass host-browser checks; a keyboard attached directly to PS5 still
needs hardware verification.

The payload locates its own installed ELF by an embedded build marker in
Payload Manager and copies it into its private runtime directory. Both grouped
and per-upload Payload Manager directories are supported.

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

- Changing the PS5 network settings can leave the web service unreachable while
  its process remains running. Restart the PS5 AI CLI payload in Payload Manager
  after changing Wi-Fi/LAN settings. Reopening the shortcut alone does not
  restart the service; automatic network recovery is not yet implemented.
- Native PTY creation returns `ENOSYS`; a shell is not bundled.
- Ordinary kernel execution rejects the tested SDK ELF. The separate child
  loader has not been integrated into Codex command execution.
- Codex device login failed at hostname resolution in the earlier connectivity
  test, and the expected system CA paths were absent. The terminal build now
  bundles certificate roots; end-to-end login remains unverified.
  Isolated curl/OpenSSL HTTPS checks passed with supplied roots and addresses;
  this does not verify Codex login, token exchange or model requests.
- The app-server uses capability-token authentication. Missing or incorrect
  tokens return HTTP 401; browser Origin requests return 403. A browser gateway
  and PS5 interface are under development.
- PS5 keyring requests return Unsupported; upstream file and ephemeral auth
  stores remain available. PATH aliases are also unsupported.

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
