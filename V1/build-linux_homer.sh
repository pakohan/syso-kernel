#!/bin/bash
rm -rf ../linux-3.10.16/usr/files
mkdir ../linux-3.10.16/usr/files
rm ../linux-3.10.16/usr/.initramfs_data.*
rm ../linux-3.10.16/initramfsconfig

gcc -static -o ../linux-3.10.16/usr/files/hello init.c -m32
cp /home/public/SYSO/busybox ../linux-3.10.16/usr/files/
cp initramfs/.profile ../linux-3.10.16/usr/files/
cp initramfs/etc/inittab ../linux-3.10.16/usr/files/
cp initramfs/etc/init.d/rcS ../linux-3.10.16/usr/files/

#cp -R ../webmin-1.660 ../linux-3.10.16/usr/initramfs/usr/webmin/

#cd ../perl-5.8.9
#make CC=../buildroot-2013.08.1/output/host/usr/bin/i386-buildroot-linux-uclibc-gcc ARCH=i386 CROSS_COMPILE=i386-buildroot-linux-uclibc- -f Makefile.micro
#cp microperl ../linux-3.10.16/usr/initramfs/bin

cp initramfs-homer ../linux-3.10.16/initramfsconfig

cd ../linux-3.10.16/
cp ../V1/config-linux .config
make ARCH=i386
cd ../V1
qemu -kernel ../linux-3.10.16/arch/x86/boot/bzImage -append "root=/dev/ram init=/init" -curses #-qmp tcp:localhost:4444,server,nowait
