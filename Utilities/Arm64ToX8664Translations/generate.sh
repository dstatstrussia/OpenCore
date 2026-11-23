#!/bin/sh
# generate.sh - regenerate the obj form (translations.o) of the universal
# arm64 -> x86_64 translations from translations.nasm.
#
#   hex form:  nasm -f bin translations.nasm -o /dev/stdout | xxd -i
#              (the bytes of the same snippets; the arrays in translations.h
#              are checked in and are consistent with this output)
#   obj form:  translations.o, below
set -e
cd "$(dirname "$0")"

nasm -f elf64 translations.nasm -o translations.o
