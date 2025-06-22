#!/bin/bash

DEPENDENCY_LIST=("fio" "make")
BD_NAME="nullb0"
IO_DEPTH=32
VERIFY="false"
SETUP="false"
PERF="false"
PLOTS="false"
JOBS_NUM=8
BRD_SIZE=400

# Function to display help
usage() {
	echo -e "Main orchestration script for all test suites and performance analysis.\n\nCan be used to:\n - run the test scripts\n - setup the experiment environment\n - generate plots based on auto-tests\n - generate the call-stack analysis flamegraphs.\n\nUsage: $0 [-v|--verify] [-s|--setup] [-p|--perf] [-c|--cplots] [-a|--auto] [--bd_name name_without_/dev/] [--io_depth number] [--jobs_num number]"
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
		--bd_name)
            BD_NAME="$2"
            shift 
            ;;
		--io_depth)
			IO_DEPTH="$2"
			shift 
			;;
		--jobs_num)
			JOBS_NUM="$2"
			shift 
			;;
		--init)
			BRD_SIZE="$2"
			shift
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

echo -e "\n\n\n--- CONFIGURATION: ---\n\n\n"
echo "Underlying block device name: $BD_NAME"
echo "Verify option: $VERIFY"
echo "IO depth: $IO_DEPTH"
echo "Jobs number: $JOBS_NUM"


if [ "$(id -u)" -ne 0 ]; then
    echo "This script should be run from a root."
    exit 1
fi

if [ "$SETUP" == "true" ]; then
	# Setup the machine for performance testing
	./setup.sh --bd_name "$BD_NAME" --io_depth "$IO_DEPTH"
fi

if [ "$AUTO_TEST" == "true" ]; then
	# Run auto fio tests ;) 
	./autotest.sh --jobs_num "$JOBS_NUM" --io_depth "$IO_DEPTH" --brd_size "$BRD_SIZE"
fi

if [ "$PLOTS" == "true" ]; then
	sudo ./plots.sh  --io_depth "$IO_DEPTH" --jobs_num "$JOBS_NUM" --brd_size "$BRD_SIZE"
fi


if [ "$PERF" == "true" ]; then
	echo -e "\nPerfofm a block device warm up"
	make fio_perf_w_opt ID=64 NJ=1 

	### Run config setup script
	if [ "$VERIFY" == "true" ]; then
		./perf.sh --io_depth "$IO_DEPTH" --jobs_num "$JOBS_NUM" -v
	else
		./perf.sh --io_depth "$IO_DEPTH" --jobs_num "$JOBS_NUM"
	fi
fi
