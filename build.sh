#!/bin/bash
BOLD='\033[1m'
GRN='\033[01;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[01;31m'
RST='\033[0m'
_timed_build() {
  _runtime=$( time ( schedtool -B -n 1 -e ionice -n 1 "$@" 2>&1 ) 3>&1 1>&2 2>&3 ) || _runtime=$( time ( "$@" 2>&1 ) 3>&1 1>&2 2>&3 )
}
script_echo() {
    echo "  $1"
}
build_kernel_package() {
    script_echo " "
    echo -e "${GRN}"
    read -p "Write the Kernel version: " KV
    echo -e "${YELLOW}"
    script_echo 'Building Custom Kernel For G513IC'
    _timed_build make -j16 debian_defconfig 2>&1 | sed 's/^/     /'
    rm -rf .version
    touch .version
    echo "$KV" >> .version
    _timed_build make -j16 bindeb-pkg 2>&1 | sed 's/^/     /'
    SUCCESS=$?
    echo -e "${RST}"

    if [ $SUCCESS -eq 0 ] && [ -f "$IMAGE" ]
    then
        echo -e "${GRN}"
        script_echo "------------------------------------------------------------"
        script_echo "Build successful..."
        script_echo  "------------------------------------------------------------"
    elif [ $SUCCESS -eq 130 ]
    then
        echo -e "${RED}"
        script_echo "------------------------------------------------------------"
        script_echo "Build force stopped by the user."
        script_echo "------------------------------------------------------------"
        echo -e "${RST}"
    elif [ $SUCCESS -eq 1 ]
    then
        echo -e "${RED}"
        script_echo "------------------------------------------------------------"
        script_echo "Build failed..check build logs for errors"
        script_echo "------------------------------------------------------------"
        echo -e "${RST}"
    fi
}
build_kernel_package