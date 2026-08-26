savedcmd_nvkvm-guest.o := ld -m elf_x86_64 -z noexecstack --no-warn-rwx-segments   -r -o nvkvm-guest.o @nvkvm-guest.mod  ; /usr/src/linux-headers-7.0.0-29-generic/tools/objtool/objtool --hacks=jump_label --hacks=noinstr --hacks=skylake --retpoline --rethunk --sls --stackval --static-call --uaccess --prefix=16  --link  --module nvkvm-guest.o

nvkvm-guest.o: $(wildcard /usr/src/linux-headers-7.0.0-29-generic/tools/objtool/objtool)
