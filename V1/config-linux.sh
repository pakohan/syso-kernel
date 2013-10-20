#!/bin/bash
cd ../linux-3.10.16/
cp ../V1/config-linux .config
make nconfig ARCH=i386
cp .config ../V1/config-linux