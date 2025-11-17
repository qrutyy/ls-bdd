#!/bin/bash

kernel_version=$(uname -r)

if echo "$kernel_version" | grep -q "fc[0-9]"; then 
    pm="dnf"
    perf_pkg="perf"
    venv_pkg="python3-virtualenv"
else
    pm="apt"
    perf_pkg="linux-tools-common linux-tools-generic linux-tools-$(uname -r)"
    venv_pkg="python3-virtualenv"
fi 

echo -e "Installing perf"
sudo "$pm" install -y "$perf_pkg"

echo -e "Installing fio and make"
sudo "$pm" install -y fio make

echo -e "Installing virtualenv"
sudo "$pm" install -y $venv_pkg

virtualenv venv --python=python3 ..
echo "Run 'source ../venv/bin/activate' to activate the virtual environment."

