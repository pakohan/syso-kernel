#!/bin/bash
rm -rf ../linux-3.10.16/usr/initramfs
rm ../linux-3.10.16/usr/.initramfs_data.*
rm ../linux-3.10.16/initramfsconfig

gcc -static -o ./initramfs/bin/hello init.c -m32

cp -R initramfs ../linux-3.10.16/usr

rm initramfs/bin/hello

#cp -R ../webmin-1.660 ../linux-3.10.16/usr/initramfs/usr/webmin/

#cd ../perl-5.8.9
#make CC=../buildroot-2013.08.1/output/host/usr/bin/i386-buildroot-linux-uclibc-gcc ARCH=i386 CROSS_COMPILE=i386-buildroot-linux-uclibc- -f Makefile.micro
#cp microperl ../linux-3.10.16/usr/initramfs/bin

cd ../busybox-1.21.1/
cp ../V1/config-busybox .config
make 
make CONFIG_PREFIX=../linux-3.10.16/usr/initramfs install

cd ../linux-3.10.16/
sh scripts/gen_initramfs_list.sh usr/initramfs/ > initramfsconfig
cat ../V1/initramfsconfig_nodes >> initramfsconfig

cp ../V1/config-linux .config
make ARCH=i386
cd ../V1
qemu -kernel ../linux-3.10.16/arch/x86/boot/bzImage -append "root=/dev/ram init=/init" -curses #-qmp tcp:localhost:4444,server,nowait
