# Object files to compile
obsidianfs-objs := inode.o file.o pageops.o

# Tell the kernel build system to build our module
obj-m += obsidianfs.o

# Kernel build directory
KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

# Quick test commands (run as root)
load:
	sudo insmod obsidianfs.ko
	sudo mkdir -p /tmp/obsidian
	sudo mount -t obsidianfs none /tmp/obsidian

unload:
	sudo umount /tmp/obsidian
	sudo rmmod obsidianfs
