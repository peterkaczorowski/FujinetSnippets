#!/bin/bash
# Build NET002_SSH — FujiNet SSH Terminal for Atari 8-bit
# Requires: cc65 toolchain (cl65, ca65, ld65)

set -e

cd "$(dirname "$0")"
mkdir -p build

cl65 -t atari -o build/ssh_terminal.com src/ssh_terminal.c src/intr.s src/hsio.s
cl65 -t atari -o build/ssh.com src/ssh.c src/intr.s
cl65 -t atari -o build/hsioset.com src/hsioset.c src/hsio.s

echo "Built:"
echo "  ssh_terminal.com  $(wc -c < build/ssh_terminal.com | tr -d ' ') bytes (SSH + HSIO)"
echo "  ssh.com           $(wc -c < build/ssh.com | tr -d ' ') bytes (SSH only)"
echo "  hsioset.com       $(wc -c < build/hsioset.com | tr -d ' ') bytes (HSIO installer)"
