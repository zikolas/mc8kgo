#!/bin/sh
# assemble MC8KGO.COM (same on-box: C:\NASM nasm -f bin MC8KGO.ASM -o MC8KGO.COM)
set -e
cd "$(dirname "$0")"
nasm -f bin MC8KGO.ASM -o MC8KGO.COM
ls -l MC8KGO.COM
