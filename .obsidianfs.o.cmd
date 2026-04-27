savedcmd_obsidianfs.o := ld -m elf_x86_64 -z noexecstack --no-warn-rwx-segments   -r -o obsidianfs.o @obsidianfs.mod  ; /home/savio/WSL2-Linux-Kernel/tools/objtool/objtool --hacks=jump_label --hacks=noinstr --hacks=skylake --ibt --retpoline --rethunk --stackval --static-call --uaccess --prefix=16  --link  --module obsidianfs.o

obsidianfs.o: $(wildcard /home/savio/WSL2-Linux-Kernel/tools/objtool/objtool)
