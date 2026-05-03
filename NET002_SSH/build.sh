#!/bin/bash
# Build NET002_SSH — FujiNet SSH Terminal for Atari 8-bit
# Requires: cc65 toolchain (cl65, ca65, ld65)

set -e

cd "$(dirname "$0")"
mkdir -p build

cl65 -t atari -o build/ssh_terminal.com src/ssh_terminal.c src/intr.s src/hsio.s
cl65 -t atari -o build/ssh.com src/ssh.c src/intr.s
cl65 -t atari -o build/sshtest.com -DHARDCODED_LOGIN src/ssh.c src/intr.s
cl65 -t atari -o build/sshtest2.com -DHARDCODED_KEYAUTH src/ssh.c src/intr.s
cl65 -t atari -o build/sshkgen.com src/sshkgen.c
cl65 -t atari -o build/sshkgen2.com src/sshkgen2.c
cl65 -t atari -o build/sshcpid.com src/sshcpid.c
cl65 -t atari -o build/hsioset.com src/hsioset.c src/hsio.s

echo "Built:"
echo "  ssh_terminal.com  $(wc -c < build/ssh_terminal.com | tr -d ' ') bytes (SSH + HSIO)"
echo "  ssh.com           $(wc -c < build/ssh.com | tr -d ' ') bytes (SSH only)"
echo "  sshtest.com       $(wc -c < build/sshtest.com | tr -d ' ') bytes (SSH auto-login: password)"
echo "  sshtest2.com      $(wc -c < build/sshtest2.com | tr -d ' ') bytes (SSH auto-login: publickey)"
echo "  sshkgen.com       $(wc -c < build/sshkgen.com | tr -d ' ') bytes (SSH key generator)"
echo "  sshkgen2.com      $(wc -c < build/sshkgen2.com | tr -d ' ') bytes (SSH key generator: overwrite)"
echo "  sshcpid.com       $(wc -c < build/sshcpid.com | tr -d ' ') bytes (SSH copy-id: test@192.168.1.20)"
echo "  hsioset.com       $(wc -c < build/hsioset.com | tr -d ' ') bytes (HSIO installer)"
