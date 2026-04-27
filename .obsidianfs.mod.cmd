savedcmd_obsidianfs.mod := printf '%s\n'   inode.o file.o | awk '!x[$$0]++ { print("./"$$0) }' > obsidianfs.mod
