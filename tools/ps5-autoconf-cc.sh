#!/usr/bin/env bash
set -euo pipefail
# The SDK wrapper adds crt1.o even during preprocessing. Configure/make need
# preprocessing to leave startup objects out while retaining SDK headers.
for argument in "$@"; do
  if [[ "$argument" == -E ]]; then
    exec /opt/ps5-payload-sdk/bin/prospero-clang -nostartfiles "$@"
  fi
done
exec /opt/ps5-payload-sdk/bin/prospero-clang "$@"
