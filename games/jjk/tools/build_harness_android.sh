#!/bin/sh
# JJK uses the same Ryujinx Maxwell-to-SPIR-V translator as P5R; only the game's extraction path differs.
# Run from the repository root inside the .NET 10 Linux container.
set -eu
exec sh games/p5r/tools/build_harness_android.sh
