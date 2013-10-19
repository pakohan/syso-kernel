#!/bin/bash
cd ../busybox-1.21.1/
cp ../V1/config-busybox .config
make menuconfig
cp .config ../V1/config-busybox