#!/usr/bin/env bash
set -euo pipefail
cd /work
python3 tools/fetch-shell.py --offline
python3 tools/fetch-loader.py --offline
mkdir -p build/shell build/dash
cc=/opt/ps5-payload-sdk/bin/prospero-clang
trace=()
if [[ "${PS5_TOOL_TRACE:-0}" == 1 ]]; then trace=(-DPS5_TOOL_TRACE); fi
"$cc" -O2 -Wall -Wextra -Werror "${trace[@]}" -c platform/freebsd11.c -o build/shell/compat.o
"$cc" -c platform/syscalls.S -o build/shell/syscalls.o
for source in sdk-elf sdk-spawn shell-exec; do
  "$cc" -O2 -Wall -Wextra -Werror "${trace[@]}" -c "platform/$source.c" -o "build/shell/$source.o"
done
for source in elfldr pt; do
  "$cc" -O2 -Wall -Werror "${trace[@]}" -c "vendor/shsrv/$source.c" -o "build/shell/$source.o"
done
mapfile -t wrappers < platform/syscalls.link
objects=(/work/build/shell/{compat,syscalls,sdk-elf,sdk-spawn,shell-exec,elfldr,pt}.o)
links="${objects[*]} ${wrappers[*]} -Wl,--wrap=fcntl -Wl,--wrap=sysctl -lpthread -ldl -lunwind -lkernel_sys"
if [[ ! -f vendor/dash/configure ]]; then (cd vendor/dash && autoreconf -fi); fi
cd build/dash
CC=/work/tools/ps5-autoconf-cc.sh CC_FOR_BUILD=clang CFLAGS='-O2 -DJOBS=0' \
  ac_cv_func_killpg=yes ac_cv_func_memfd_create=no ac_cv_func_tee=no ac_cv_func_faccessat=no \
  /work/vendor/dash/configure --host=x86_64-unknown-freebsd \
    --build="$(clang -dumpmachine)" --without-libedit --disable-fnmatch \
    --disable-glob --disable-tee --disable-memfd-create
# Make's ordinary host generator would embed Linux signal numbers.
mkdir -p src
python3 /work/tools/dash-signames.py
# The proxy/loader objects are linked through LDFLAGS, so Automake would not
# otherwise relink Dash when those objects change.
make -j2 LDFLAGS="$links" EXTRA_dash_DEPENDENCIES="${objects[*]}"
cp src/dash /work/build/shell/sh.elf
llvm-strip --strip-all /work/build/shell/sh.elf
python3 /work/tools/payload.py /work/build/shell/sh.elf
cd /work/vendor/sbase
make -j2 CC=/work/tools/ps5-autoconf-cc.sh AR=/opt/ps5-payload-sdk/bin/prospero-ar \
  RANLIB=llvm-ranlib CFLAGS='-O2 -D__BSD_VISIBLE=1' libutf.a libutil.a
tool_objects=()
while IFS= read -r name; do
  object="/work/build/shell/sbase-$name.o"
  "$cc" -O2 -D__BSD_VISIBLE=1 -D_BSD_SOURCE -D_XOPEN_SOURCE=700 \
    -Dmain="sbase_${name}_main" -c "$name.c" -o "$object"
  tool_objects+=("$object")
done < /work/platform/runtime-tools.txt
cd /work
python3 tools/sbase-dispatch.py
# execvp is wrapped only in these single-threaded applets, never in Codex.
"$cc" -O2 build/shell/sbase-box.c platform/tool-stdio.c "${tool_objects[@]}" \
  vendor/sbase/libutil.a vendor/sbase/libutf.a "${objects[@]}" "${wrappers[@]}" \
  -Wl,--wrap=fcntl -Wl,--wrap=sysctl -Wl,--wrap=execvp -lpthread -ldl -lunwind -lkernel_sys \
  -o build/shell/sbase-box.elf
llvm-strip --strip-all build/shell/sbase-box.elf
python3 tools/payload.py build/shell/sbase-box.elf
