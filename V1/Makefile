-include variables
-include ../server.host

#TARGET=../${KERNEL_VERSION}/arch/${ARCH}/boot/bzImage
USERPROG=../${KERNEL_VERSION}/usr/initramfs/bin/hello
ROOTFS=../${KERNEL_VERSION}/usr/initramfs
LINUX_CONFIG=../${KERNEL_VERSION}/.config
BUSYBOX_CONFIG=../busybox-1.21.1/.config
BUSYBOX=../${KERNEL_VERSION}/usr/initramfs/bin/busybox

.PHONY: all clean clean-rootfs config-linux config-busybox start

.NOTPARALLEL:

all: ${TARGET}

start: ${TARGET}
	${QEMU} -kernel ${TARGET} -qmp tcp:localhost:25565,server,nowait

config-linux:
	cp config-linux ../${KERNEL_VERSION}/.config
	make -C ../${KERNEL_VERSION} ARCH=${ARCH} nconfig
	cp ../${KERNEL_VERSION}/.config config-linux

config-busybox:
	cp config-busybox ../busybox-1.21.1/.config
	make -C ../busybox-1.21.1 menuconfig
	cp ../busybox-1.21.1/.config config-busybox

defconfig-linux:
	cp config-linux ../${KERNEL_VERSION}/.config
	make -C ../${KERNEL_VERSION} ARCH=${ARCH} ${DEFCONFIG}
	cp ../${KERNEL_VERSION}/.config config-linux

${TARGET}: ${ROOTFS} ${USERPROG} ${LINUX_CONFIG} ${BUSYBOX} 
ifdef HOMER
	cp initramfs-homer ../${KERNEL_VERSION}/initramfsconfig
else
	sh ../${KERNEL_VERSION}/scripts/gen_initramfs_list.sh ../${KERNEL_VERSION}/usr/initramfs/ > ../${KERNEL_VERSION}/initramfsconfig
	cat initramfsconfig_nodes >> ../${KERNEL_VERSION}/initramfsconfig
endif
	make -j ${JOBS} -C ../${KERNEL_VERSION} ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}

${ROOTFS}:
	cp -R ../V1/initramfs ../${KERNEL_VERSION}/usr

${BUSYBOX}: ${ROOTFS} ${BUSYBOX_CONFIG}
ifdef HOMER
	mkdir -p ${ROOTFS}/bin/
	cp /home/public/SYSO/busybox ${ROOTFS}/bin/busybox
else
	make -j ${JOBS} -C ../busybox-1.21.1 EXTRA_CFLAGS=${EXTRA_CFLAGS} EXTRA_LDFLAGS=${EXTRA_LDFLAGS} ARCH=${ARCH}
	make -C ../busybox-1.21.1 EXTRA_CFLAGS=${EXTRA_CFLAGS} EXTRA_LDFLAGS=${EXTRA_LDFLAGS} CONFIG_PREFIX=$< install
endif


${USERPROG}: ${ROOTFS} init.c
	mkdir -p ${ROOTFS}/bin/
	${CROSS_COMPILE}gcc -static ${EXTRA_CFLAGS} init.c -o $@

${BUSYBOX_CONFIG}:
	cp config-busybox ../busybox-1.21.1/.config

${LINUX_CONFIG}:
	cp config-linux ../${KERNEL_VERSION}/.config

clean: clean-rootfs
	make -j ${JOBS} -C ../${KERNEL_VERSION} distclean
	make -j ${JOBS} -C ../busybox-1.21.1 distclean
	rm -rf ${USERPROG}
	rm -rf ${ROOTFS}

clean-rootfs:
	rm -rf ../${KERNEL_VERSION}/usr/initramfs
	rm -f ../${KERNEL_VERSION}/usr/.initramfs_data.*
	rm -f ../${KERNEL_VERSION}/initramfsconfig
