#binary-only fuzzing with dynamorio and afl

##Installation
1. download, compile and install afl => https://github.com/vanhauser-thc/AFLplusplus
2. download, compile and install dyninst => https://github.com/dyninst/dyninst
3. download, compile and install afl-dyninst  => https://github.com/vanhauser-thc/afl-dyninst
4. download, compile and install dynamorio => https://github.com/DynamoRIO/dynamorio
5. make a symlink to the afl folder here named "afl" , e.g. "ln -s ../afl afl"
6. export DYNAMORIO_HOME=/path/to/dynamorio/build directory
7. make
8. make install


##How to run
1. afl-dyninst.sh -i program -o program_instrumented -D
It is a good idea to add -e and -E with well selected function addresses to
make the fuzzing faster
NOTE: you can skip this step and use -forkserver option in the next step.
But this is slower at the moment!

2. afl-fuzz-dynamorio.sh [normal afl-fuzz options]
That's it! If you fuzzing does not run, afl-fuzz might need more memory, set
AFL_MEM to a high value, e.g. 700 for 700MB


##When to use it
when normal afl-dyninst is crashing the binary and qemu mode -Q is not
an option.
Dynamorio is x10 slower than Qemu, 25x slower than dyninst - however 10x
faster than Pintool, and works additionally on ARM and AARCH64.
In memory fuzzing (function fuzzing) is a much faster option and implemented
in a future release.


##Who and where
https://github.com/vanhauser-thc/afl-dynamorio

Marc "van Hauser" Heuse <mh@mh-sec.de> || <vh@thc.org>

