-include variables
-include ../server.host

TARGET=../linux-3.10.16/arch/${ARCH}/boot/bzImage
USERPROG=../linux-3.10.16/usr/initramfs/bin/hello
ROOTFS=../linux-3.10.16/usr/initramfs
LINUX_CONFIG=../linux-3.10.16/.config
BUSYBOX_CONFIG=../busybox-1.21.1/.config
BUSYBOX=../linux-3.10.16/usr/initramfs/bin/busybox

.PHONY: all clean clean-rootfs config-linux config-busybox start

.NOTPARALLEL:

all: ${TARGET}

start: ${TARGET}
	qemu -kernel ../linux-3.10.16/arch/${ARCH}/boot/bzImage -append "root=/dev/ram init=/init" -curses -qmp tcp:localhost:4444,server,nowait

config-linux:
	cp ../V1/config-linux ../linux-3.10.16/.config
	make -C ../linux-3.10.16 nconfig ARCH=${ARCH}
	cp ../linux-3.10.16/.config ../V1/config-linux

config-busybox:
	cp ../V1/config-busybox ../busybox-1.21.1/.config
	make -C ../busybox-1.21.1 menuconfig
	cp ../busybox-1.21.1/.config ../V1/config-busybox

${TARGET}: ${ROOTFS} ${USERPROG} ${LINUX_CONFIG} ##${BUSYBOX} 
ifdef HOMER
	cp initramfs-homer ../linux-3.10.16/initramfsconfig
else
	sh ../linux-3.10.16/scripts/gen_initramfs_list.sh ../linux-3.10.16/usr/initramfs/ > ../linux-3.10.16/initramfsconfig
	cat initramfsconfig_nodes >> ../linux-3.10.16/initramfsconfig
endif
	make -j ${JOBS} -C ../linux-3.10.16 ARCH=${ARCH}

${ROOTFS}:
	cp -R ../V1/initramfs ../linux-3.10.16/usr

${BUSYBOX}: ${ROOTFS} ${BUSYBOX_CONFIG}
ifdef HOMER
	mkdir -p ${ROOTFS}/bin/
	cp /home/public/SYSO/busybox ${ROOTFS}/bin/busybox
else
	make -j ${JOBS} -C ../busybox-1.21.1 EXTRA_CFLAGS=${EXTRA_CFLAGS} EXTRA_LDFLAGS=${EXTRA_LDFLAGS}
	make -C ../busybox-1.21.1 EXTRA_CFLAGS=${EXTRA_CFLAGS} EXTRA_LDFLAGS=${EXTRA_LDFLAGS} CONFIG_PREFIX=$< install
endif


${USERPROG}: ${ROOTFS} init.c
	mkdir -p ${ROOTFS}/bin/
	gcc -static ${EXTRA_CFLAGS} init.c -o $@

${BUSYBOX_CONFIG}:
	cp config-busybox ../busybox-1.21.1/.config

${LINUX_CONFIG}:
	cp config-linux ../linux-3.10.16/.config

clean: clean-rootfs
	make -j ${JOBS} -C ../linux-3.10.16 distclean
	make -j ${JOBS} -C ../busybox-1.21.1 distclean
	rm -rf ${USERPROG}
	rm -rf ${ROOTFS}

clean-rootfs:
	rm -rf ../linux-3.10.16/usr/initramfs
	rm -f ../linux-3.10.16/usr/.initramfs_data.*
	rm -f ../linux-3.10.16/initramfsconfig
