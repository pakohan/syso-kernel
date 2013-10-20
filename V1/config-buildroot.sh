#!/bin/bash
cd ../buildroot-2013.08.1/
cp ../V1/config-buildroot .config
make menuconfig
cp .config ../V1/config-buildroot