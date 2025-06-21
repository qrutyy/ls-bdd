#!/bin/bash

echo -e "Install perf"
sudo apt-get install linux-tools-common linux-tools-generic linux-tools-"$(uname -r)"

echo -e "Install fio and make"
sudo apt install fio make

