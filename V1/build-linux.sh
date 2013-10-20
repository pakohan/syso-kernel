#!/bin/bash
rm -rf ../linux-3.10.16/usr/initramfs
rm ../linux-3.10.16/usr/.initramfs_data.*
rm ../linux-3.10.16/initramfsconfig

mkdir ../linux-3.10.16/usr/initramfs
mkdir ../linux-3.10.16/usr/initramfs/bin
mkdir ../linux-3.10.16/usr/initramfs/etc
mkdir ../linux-3.10.16/usr/initramfs/etc/init.d/
mkdir ../linux-3.10.16/usr/initramfs/dev
mkdir ../linux-3.10.16/usr/initramfs/proc
mkdir ../linux-3.10.16/usr/initramfs/usr
mkdir ../linux-3.10.16/usr/initramfs/var
mkdir ../linux-3.10.16/usr/initramfs/sys
mkdir ../linux-3.10.16/usr/initramfs/tmp

cp inittab ../linux-3.10.16/usr/initramfs/etc
cp rcS ../linux-3.10.16/usr/initramfs/etc/init.d/
cp profile ../linux-3.10.16/usr/initramfs/.profile

#cp -R ../webmin-1.660 ../linux-3.10.16/usr/initramfs/usr/webmin/

#cd ../perl-5.8.9
#make CC=../buildroot-2013.08.1/output/host/usr/bin/i386-buildroot-linux-uclibc-gcc ARCH=i386 CROSS_COMPILE=i386-buildroot-linux-uclibc- -f Makefile.micro
#cp microperl ../linux-3.10.16/usr/initramfs/bin

cd ../busybox-1.21.1/
cp ../V1/config-busybox .config
make 
make CONFIG_PREFIX=../linux-3.10.16/usr/initramfs install

cd ../V1
gcc -static -o ../linux-3.10.16/usr/initramfs/bin/hello init.c -m32

cd ../linux-3.10.16/
sh scripts/gen_initramfs_list.sh usr/initramfs/ > initramfsconfig
cat ../V1/initramfsconfig_nodes >> initramfsconfig

cp ../V1/config-linux .config
make ARCH=i386 && cd ../V1 && qemu -kernel ../linux-3.10.16/arch/x86/boot/bzImage -append "root=/dev/ram init=/init" -curses
