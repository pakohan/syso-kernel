#!/bin/bash
FOLDER = ${PWD##*/}
cd ../linux-3.10.16/
cp ../$FOLDER/config-linux .config
make nconfig ARCH=arm
cp .config ../$FOLDER/config-linux