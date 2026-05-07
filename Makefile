# Object files to compile
obsidianfs-objs := inode.o file.o pageops.o ioctlops.o

# Tell the kernel build system to build our module
obj-m += obsidianfs.o

# Kernel build directory
KDIR := /lib/modules/$(shell uname -r)/build

all: module userspace
	
module:
	make -C $(KDIR) M=$(PWD) modules

userspace:
	gcc obsidiancommand.c -o commandBin

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f commandBin
# Quick test commands (run as root)
load:
	sudo insmod obsidianfs.ko
	sudo mkdir -p /tmp/obsidian
	sudo mount -t obsidianfs none /tmp/obsidian

unload:
	sudo umount /tmp/obsidian
	sudo rmmod obsidianfs
