#!/bin/bash
rm -rf ../linux-3.10.16/usr/initramfs
mkdir ../linux-3.10.16/usr/initramfs
mkdir ../linux-3.10.16/usr/initramfs/bin
mkdir ../linux-3.10.16/usr/initramfs/etc
mkdir ../linux-3.10.16/usr/initramfs/dev

cp inittab ../linux-3.10.16/usr/initramfs/etc

cd ../busybox-1.21.1/
cp ../V1/config-busybox .config
make 
make CONFIG_PREFIX=../linux-3.10.16/usr/initramfs install

cd ../V1
gcc -static -o hello init.c -m32
mv hello ../linux-3.10.16/usr/initramfs/bin

cd ../linux-3.10.16/
sh scripts/gen_initramfs_list.sh usr/initramfs/ > ../V1/initramfsconfig
cat ../V1/initramfsconfig_nodes >> ../V1/initramfsconfig

cp ../V1/config-linux .config
make ARCH=i386
cd ../V1
qemu -curses -kernel ../linux-3.10.16/arch/x86/boot/bzImage -append "root=/dev/ram init=/sbin/init"
