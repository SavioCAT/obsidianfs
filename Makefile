# Object files to compile
obsidianfs-objs := inode.o file.o

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
	sudo mkdir -p /mnt/obsidian
	sudo mount -t obsidianfs none /mnt/obsidian

unload:
	sudo umount /mnt/obsidian
	sudo rmmod obsidianfs

test: load
	echo "hello obsidianfs" | sudo tee /mnt/obsidian/test.txt
	cat /mnt/obsidian/test.txt
	ls -lai /mnt/obsidian/
