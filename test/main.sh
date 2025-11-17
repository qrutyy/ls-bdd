#!/bin/bash

source ./configurable_params.sh

readonly DEPENDENCY_LIST=("fio" "make")

VERIFY="false"
SETUP="false"
PERF="false"
PLOTS="false"

# Function to display help
usage() {
	echo -e "Main orchestration script for all test suites and performance analysis.\n\nCan be used to:\n - run the test scripts\n - setup the experiment environment\n - generate plots based on auto-tests\n - generate the call-stack analysis flamegraphs.\n\nUsage: $0 [-v|--verify] [-s|--setup] [-p|--perf] [-c|--cplots] [-a|--auto] "
    exit 1
}

# Function to validate VERIFY variable
validate_verify_input() {
    if [[ "$VERIFY" != "true" && "$VERIFY" != "false" ]]; then
        echo "ERROR: VERIFY must be either 'true' or 'false'."
        usage
    fi
}

install_deps() {
	echo -e "Installing dependencies:\n"
	./install.sh
}

check_package() {
	if ! command -v "$1" &> /dev/null; then
		echo "Error: '$1' is not installed. Please install it and try again."
		./install.sh
	fi
}

check_dependencies() {
	for package in "${DEPENDENCY_LIST[@]}"; do
		check_package "$package"
	done
}

# Parse options using getopts
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -v|--verify)
            VERIFY="true"
            ;;
		-s|--setup)
            SETUP="true"
            ;;
		-p|--perf)
            PERF="true"
            ;;
		-c|--cplots)
			PLOTS="true"
			;;
		-a|--auto)
			AUTO_TEST="true"
			;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
	shift
done

install_deps
check_dependencies
validate_verify_input

free -m
mkdir -p plots logs

output_config

if [ "$(id -u)" -ne 0 ]; then
    echo "This script should be run from a root."
    exit 1
fi

if [ "$SETUP" == "true" ]; then
	./setup.sh 
fi

if [ "$AUTO_TEST" == "true" ]; then
	./autotest.sh
fi

if [ "$PLOTS" == "true" ]; then
	sudo ./plots.sh  
fi

if [ "$PERF" == "true" ]; then
	echo -e "\nPerfofm a block device warm up"
	make fio_perf_w_opt ID="$IO_DEPTH" NJ="$JOBS_NUM"

	### Run config setup script
	if [ "$VERIFY" == "true" ]; then
		./perf.sh -v
	else
		./perf.sh 
	fi
fi
