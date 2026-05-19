obsidianfs-objs := super.o inode.o file.o pageops.o ioctlops.o

obj-m += obsidianfs.o

KDIR := /lib/modules/$(shell uname -r)/build

.PHONY: all module userspace clean load unload

all: module userspace

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace:
	gcc obsidiancommand.c -o commandBin

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f commandBin

load:
	sudo insmod obsidianfs.ko
	sudo mkdir -p /tmp/obsidian
	sudo mount -t obsidianfs none /tmp/obsidian

unload:
	sudo umount /tmp/obsidian
	sudo rmmod obsidianfs
