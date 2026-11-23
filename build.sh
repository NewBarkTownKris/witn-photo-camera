#!/bin/sh
set -eu
cd "$(dirname "$0")"
wine_include=${WINE_INCLUDE_DIR:-/usr/include/wine}
wine_lib=${WINE_LIBRARY_DIR:-/usr/lib/wine/x86_64-windows}
mkdir -p build
set --
for unit in mod photo folder studio optics capture exr overlay_gpu projection print_spool; do
    "${CLANG:-clang}" --target=x86_64-w64-windows-gnu -O2 -Wall -Wextra -Werror -c \
        -isystem "$wine_include/windows" -isystem "$wine_include/msvcrt" \
        -DWITN_IMAGE_SIZE=0x1e56000 -DWITN_TIMESTAMP=0x6ac752a5 "src/$unit.c" -o "build/$unit.obj"
    set -- "$@" "build/$unit.obj"
done
"${LLD_LINK:-lld-link}" /dll /entry:DllMain /nodefaultlib /export:MiniDumpWriteDump=proxy_dump \
    "/timestamp:${SOURCE_DATE_EPOCH:-0}" \
    /out:build/dbghelp.dll /implib:build/dbghelp.lib "$@" \
    "$wine_lib/libkernel32.a" "$wine_lib/libuser32.a" "$wine_lib/libgdi32.a" "$wine_lib/libshell32.a" \
    "$wine_lib/libd3d12.a" "$wine_lib/libole32.a" "$wine_lib/libucrtbase.a" "$wine_lib/libmsvcrt.a"
