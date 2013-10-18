#!/bin/bash
gcc -static -o ../linux-3.10.16/usr/init init.c -m32
cd ../linux-3.10.16
#make mrproper ARCH=i386
#make allnoconfig ARCH=i386
cp ../V1/.config .
make ARCH=i386
cd ../V1
qemu -curses -kernel ../linux-3.10.16/arch/x86/boot/bzImage -append "root=/dev/ram init=/init"
