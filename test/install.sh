#!/bin/bash

kernel_version=$(uname -r)

if "$kernel_version" | grep -q "fc[0-9]"; then 
	pm="dnf"
else
	pm="apt"
fi 

echo -e "Install perf"
sudo "$pm" install linux-tools-common linux-tools-generic linux-tools-"$(uname -r)"

echo -e "Install fio and make"
sudo "$pm" install fio make

virtualenv venv --python=python3 ..
sudo sh -c 'source ../venv/bin/activate && <comando_a_ejecutar>'
   

