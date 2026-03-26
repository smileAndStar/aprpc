#!/bin/bash

set -e

rm -rf build
mkdir build
cd build
cmake ..
make -j4

cd ..
cp -r src/include lib