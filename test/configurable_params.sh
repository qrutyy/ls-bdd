#!/bin/bash

##########################
### GENERAL PARAMETERS ###
##########################

JOBS_NUM=8 # number of jobs
IO_DEPTH=32
VBD_NAME="lsvbd1"
BD_NAME="nullb0"
BD_SIZE=400
BD_TYPE="lf"
BD_DS="sl"

###########################
### WORKFLOW PARAMETERS ### 
###########################

JOB_SIZE="100M"

###############################
### AUTO TESTING PARAMETERS ###
###############################

# NOTE: ramdisk can be replaced with the regular ssd/hdd, 
# just remove the line with brd module init from autotest.sh

NBD_SIZE=10 # size of underlying RAMdisk in GB.

# NOTE: this block sizes are being treated as Read BS and Write BS. 
# So they would be kinda mixed in case BS_MIX_MODE is set to 1.
BS_LIST=("4K" "8K" "16K" "32K" "64K" "128K") # list of blocvk sizes to test 

BS_MIX_MODE=0

#####################################
### SPECIFIC PLOT MODE PARAMETERS ###
#####################################

PL_RUNS=10
PL_IOPS_CONC_NJ_LIST=("1" "2" "4" "8")
PL_RW_TYPES=("rw" "randrw")
PL_RW_MIXES=("0-100" "100-0")
PL_AVAILABLE_DS=("sl" "ht")

# configs for plotter that show iops for each nj/id
# "operation (write/read) | block size"
PL_IOPS_FOR_EACH_NJID_CFG=(
	"write 8"
	"read 8"
)

# configs for general concurrent plotter 
# "block size | numjobs | iodepth | metric (IOPS/LAT)" 
PL_GENERAL_CONC_CFG=(
	"8 8 32 IOPS" 
	"8 1 1 LAT"
	"8 1 32 LAT"
)

PL_PRECOND_JOBS_NUM=10
PL_PRECOND_IODEPTH=32

output_config() {
	echo "##########################"
	echo "### CURRENT CONFIGURATION ###"
	echo "##########################"
	echo

	echo "GENERAL PARAMETERS:"
	echo "  JOBS_NUM=$JOBS_NUM"
	echo "  IO_DEPTH=$IO_DEPTH"
	echo "  VBD_NAME=$VBD_NAME"
	echo "  BD_NAME=$BD_NAME"
	echo "  BD_SIZE=$BD_SIZE"
	echo "  BD_TYPE=$BD_TYPE"
	echo "  BD_DS=$BD_DS"
	echo

	echo "WORKFLOW PARAMETERS:"
	echo "  JOB_SIZE=$JOB_SIZE"
	echo

	echo "AUTO TESTING PARAMETERS:"
	echo "  NBD_SIZE=${NBD_SIZE}G"
	echo "  BS_LIST=(${BS_LIST[*]})"
	echo "  BS_MIX_MODE=$BS_MIX_MODE"
	echo

	echo "SPECIFIC PLOT MODE PARAMETERS:"
	echo "  PL_RUNS=$PL_RUNS"
	echo "  PL_IOPS_CONC_NJ_LIST=(${PL_IOPS_CONC_NJ_LIST[*]})"
	echo "  PL_RW_TYPES=(${PL_RW_TYPES[*]})"
	echo "  PL_RW_MIXES=(${PL_RW_MIXES[*]})"
	echo "  PL_AVAILABLE_DS=(${PL_AVAILABLE_DS[*]})"
	echo "  PL_IOPS_FOR_EACH_NJID_CFG=(${PL_IOPS_FOR_EACH_NJID_CFG[*]})"
	echo "  PL_GENERAL_CONC_CFG=(${PL_GENERAL_CONC_CFG[*]})"
	echo "  PL_PRECOND_JOBS_NUM=$PL_PRECOND_JOBS_NUM"
	echo "  PL_PRECOND_IODEPTH=$PL_PRECOND_IODEPTH"
	echo
}
