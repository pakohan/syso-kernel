-include variables

TARGET=../linux-3.10.16/arch/${ARCH}/boot/bzImage
USERPROG=../linux-3.10.16/usr/initramfs/bin/hello
ROOTFS=../linux-3.10.16/usr/initramfs
LINUX_CONFIG=../linux-3.10.16/.config
BUSYBOX_CONFIG=../busybox-1.21.1/.config
BUSYBOX=../busybox-1.21.1/busybox

.PHONY: all clean clean-rootfs
all: ${TARGET}

start: ${TARGET}
	qemu -kernel ../linux-3.10.16/arch/${ARCH}/boot/bzImage -append "root=/dev/ram init=/init" -curses -qmp tcp:localhost:4444,server,nowait

${TARGET}: ${ROOTFS} ${BUSYBOX} ${USERPROG} ${LINUX_CONFIG}
	sh ../linux-3.10.16/scripts/gen_initramfs_list.sh ../linux-3.10.16/usr/initramfs/ > ../linux-3.10.16/initramfsconfig
	cat initramfsconfig_nodes >> ../linux-3.10.16/initramfsconfig
	make -C ../linux-3.10.16

${ROOTFS}:
	cp -R initramfs ../linux-3.10.16/usr

${BUSYBOX}: ${ROOTFS} ${BUSYBOX_CONFIG}
	make -C ../busybox-1.21.1
	make -C ../busybox-1.21.1 EXTRA_CFLAGS="-m32" EXTRA_LDFLAGS="-m32" CONFIG_PREFIX=$< install

${USERPROG}: ${ROOTFS} init.c
	gcc -static ${EXTRA_CFLAGS} init.c -o $@

${BUSYBOX_CONFIG}: config-busybox
	cp config-busybox ../busybox-1.21.1/.config

${LINUX_CONFIG}: config-linux
	cp config-linux ../linux-3.10.16/.config

clean:
	make -C ../linux-3.10.16 distclean
	make -C ../busybox-1.21.1 distclean
	rm -rf ${USERPROG}
	rm -rf ${ROOTFS}

clean-rootfs:
	rm -rf ../linux-3.10.16/usr/initramfs
	rm ../linux-3.10.16/usr/.initramfs_data.*
	rm ../linux-3.10.16/initramfsconfig
