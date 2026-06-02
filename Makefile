obsidianfs-objs := super.o inode.o file.o pageops.o ioctlops.o

obj-m += obsidianfs.o

KDIR := /lib/modules/$(shell uname -r)/build

.PHONY: all module userspace clean load unload

all: module userspace

module:
	$(MAKE) -C $(KDIR) M=$(PWD) CONFIG_FRAME_WARN=512 modules

userspace:
	gcc obsidiancommand.c -o commandBin
	gcc -O2 -Wall -Wextra mkfs.obsidianfs.c -o mkfs.obsidianfs

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f commandBin mkfs.obsidianfs

load:
	sudo insmod obsidianfs.ko
	dd if=/dev/zero of=./test.img bs=4096 count=6400000
	./mkfs.obsidianfs -L test ./test.img
	sudo losetup -f test.img
	mkdir /tmp/obsidianfs
	echo "Look for which loop was attributed to obsidianfs with sudo losetup -l"
	echo "After do this command: sudo mount -t obsidianfs /dev/loopX /tmp/obsidianfs"

unload:
	sudo umount /tmp/obsidianfs
	sudo rm -rf /tmp/obsidianfs
	sudo rm -rf ./test.img
	sudo rmmod obsidianfs
