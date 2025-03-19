#!/bin/bash

echo -e "Install perf"
sudo apt-get install linux-tools-common linux-tools-generic linux-tools-`uname -r`

echo -e "Install fio and make"
sudo apt install fio make

echo -e "\nInstalling py dependencies for plot generators and benchmarks"
sudo apt install python3-pandas python3-numpy python3-numpy python3-scipy python3-matplotlib

