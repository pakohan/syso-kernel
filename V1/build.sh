#!/bin/bash
cd /Users/mogli/Code/SySo/V1
rm -rf rootfs
rm init
rm ramdisk.img
#rm -rf linux-3.11.4
#tar xJf linux-3.11.4.tar.xz
cd ../linux-3.10.16
make mrproper ARCH=i386
make allnoconfig ARCH=i386
#make i386_defconfig ARCH=i386
make nconfig ARCH=i386
make -j3 ARCH=i386
cd ../V1
gcc -static -o init init.c -m32
dd if=/dev/zero of=ramdisk.img bs=1M count=8
mkfs.ext2 ramdisk.img
mkdir rootfs
sudo mount -o loop ramdisk.img rootfs
sudo cp init rootfs
sudo mkdir rootfs/dev
sudo mknod rootfs/dev/console c 5 1
sudo mknod rootfs/dev/ram b 1 0
sudo umount rootfs
qemu -kernel linux-3.11.4/arch/x86/boot/bzImage  -initrd ramdisk.img  -append "root=/dev/ram init=/init"
