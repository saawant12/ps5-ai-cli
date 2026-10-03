#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 tools/embed-ui.py
flags=(-O2 -Wall -Wextra -Werror -pthread)
sources=(tools/ui/host.c app/gateway.c app/http.c app/websocket.c)
case "$(uname -s)" in
  Darwin) flags+=(-Wno-deprecated-declarations) ;;
  Linux) sources+=(-lcrypto -lutil) ;;
  *) echo 'The local terminal test host supports macOS and Linux.' >&2; exit 1 ;;
esac
"${CC:-cc}" "${flags[@]}" "${sources[@]}" -o build/ui-terminal-host
