#!/bin/bash

DEPENDENCY_LIST=("fio" "make" "perf")
BD_NAME="ram0"
IO_DEPTH=32
VERIFY="false"
SETUP="false"
PERF="false"
PLOTS="false"
JOBS_NUM=4
BRD_SIZE=5

# Function to display help
usage() {
    echo "Usage: $0 [-v|--verify] [-s|--setup] [-p|--perf] [-c|--cplots] [--bd_name name_without_/dev/] [--io_depth number] [--jobs_num number]"
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
#echo -e "Creating RAM disk"
#modprobe brd rd_nr=1 rd_size=$((BRD_SIZE * 1048576))
#free -m
mkdir -p plots logs

echo "Block device name: $BD_NAME"
echo "Verify option: $VERIFY"

if [ "$(id -u)" -ne 0 ]; then
    echo "This script should be run from a root."
    exit 1
fi

if [ "$SETUP" == "true" ]; then
	### Run config setup script
	./setup.sh --bd_name "$BD_NAME" --io_depth "$IO_DEPTH"
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
