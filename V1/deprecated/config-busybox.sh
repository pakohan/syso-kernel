#!/bin/bash

FOLDER=${PWD##*/}

cd ../busybox-1.21.1/
cp ../$FOLDER/config-busybox .config
make menuconfig
cp .config ../$FOLDER/config-busybox
