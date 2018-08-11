#!/bin/sh
test -z "$1" -o "$1" = "-h" && {
  echo HELP for afl-fuzz-dynamorio.sh :
  echo ================================
  echo you must it run like this:
  echo "  afl-fuzz-dynamorio.sh -i in_dir -o out_dir -otheropt -- program -with -options"
  echo "Note the \"--\" - it is essential!"
  echo Note that you either have to instrument the program with :
  echo "  \"afl-dyninst -i program -o program_inst -D\""
  echo or use the option -forkserver which will will implement a fork server
  echo "in main(). You can specify a different entrypoint with -entrypoint otherfunc or 0x123456."
  echo To set a higher memory requirement, set AFL_MEM=700 for 700mb, default is 500, minimum is 350
  exit 1
}

test -z "$DYNAMORIO_HOME" && { echo Error: environment variable DYNAMORIO_HOME is not set ; exit 1 ; }
test -x "$DYNAMORIO_HOME/bin64/drrun" || { echo "Error: environment variable DYNAMORIO_HOME is not pointing to the build directory (where bin64/drrun is residing)" ; exit 1 ; }
CLIENT=
test -e ./libafl-dynamorio.so && CLIENT=./libafl-dynamorio.so 
test -z "$CLIENT" -a -e "/usr/local/lib/dynamorio/libafl-dynamorio.so" && CLIENT=/usr/local/lib/dynamorio/libafl-dynamorio.so
test -z "$CLIENT" && { echo Error: can not find libafl-dynamorio.so either in the current directory nor in /usr/local/lib/dynamorio ; exit 1 ; }

test -z "$AFL_MEM" && AFL_MEM=500

AFLDYNAMORIO=""

OPS=
LOAD=
while [ '!' "$1" = "--" ]; do
  OK=
test -z "$1" && { echo Error: no -- switch found ; exit 1 ; }
  test "$1" = "-libs" && { shift ; AFLDYNAMORIO="$AFLDYNAMORIO -libs" ; OK=1 ; }
  test "$1" = "-forkserver" && { shift ; AFLDYNAMORIO="$AFLDYNAMORIO -forkserver" ; OK=1 ; }
  test "$1" = "-entrypoint" && { shift ; AFLDYNAMORIO="$AFLDYNAMORIO -entrypoint $1" ; shift ; OK=1 ; }
  test "$1" = "-alternative" && { shift ; AFLDYNAMORIO="$AFLDYNAMORIO -alternative" ; OK=1 ; }
  test -z "$OK" && { OPS="$OPS $1" ; shift ; }
done

sysctl -w kernel.core_pattern="core" > /dev/null
sysctl -w kernel.randomize_va_space=0 > /dev/null
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null

export AFL_SKIP_BIN_CHECK=1
export DYNINSTAPI_RT_LIB=/usr/local/lib/libdyninstAPI_RT.so
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:.
export AFL_EXIT_WHEN_DONE=1
#export AFL_TMPDIR=/run/$$
#export AFL_PRELOAD=./desock.so:./libdislocator/libdislocator.so

echo Running: afl-fuzz -m $AFL_MEM $OPS -- $DYNAMORIO_HOME/bin64/drrun -c "$CLIENT" $AFLDYNAMORIO $*
sleep 1
afl-fuzz -m $AFL_MEM $OPS -- $DYNAMORIO_HOME/bin64/drrun -c "$CLIENT" $AFLDYNAMORIO $*
