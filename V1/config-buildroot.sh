#!/bin/bash

FOLDER=${PWD##*/}

cd ../buildroot-2013.08.1/
cp ../$FOLDER/config-buildroot .config
make menuconfig
cp .config ../$FOLDER/config-buildroot