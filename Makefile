CFLAGS=-Wno-pointer-to-int-cast -O3

all:	dynclean
	@test -e afl || { echo Error: we need afl to symlink to an afl source folder ; exit 1 ; }
	mkdir -p build
	cd build && cmake .. -DDynamoRIO_DIR="$(DYNAMORIO_HOME)/cmake" && make
	@gcc -o test test.c

alternative:	dynclean
	@test -e afl || { echo Error: we need afl to symlink to an afl source folder ; exit 1 ; }
	mkdir -p build
	cd build && cmake .. -DALTERNATIVE=ON -DDynamoRIO_DIR="$(DYNAMORIO_HOME)/cmake" && make
	@gcc -o test test.c

debug:	dynclean
	@test -e afl || { echo Error: we need afl to symlink to an afl source folder ; exit 1 ; }
	mkdir -p build
	cd build && cmake .. -DDEBUG=ON -DDynamoRIO_DIR="$(DYNAMORIO_HOME)/cmake" && make
	@gcc -o test test.c

test:	dynclean
	@test -e afl || { echo Error: we need afl to symlink to an afl source folder ; exit 1 ; }
	mkdir -p build
	cd build && cmake .. -DTESTER=ON -DDynamoRIO_DIR="$(DYNAMORIO_HOME)/cmake" && make
	@gcc -o test test.c

install:
	install -d /usr/local/lib/dynamorio
	install build/libafl-dynamorio.so /usr/local/lib/dynamorio
	install afl-fuzz-dynamorio.sh /usr/local/bin

save:	all
	mkdir -p bin/`uname -m`
	cp -v build/lib*.so bin/`uname -m`
	-@svn add bin/`uname -m` 2> /dev/null
	-@svn add bin/`uname -m`/*.so 2> /dev/null
	-svn commit -m "`uname -m`"

dynclean:
	rm -f build/*.*

clean:
	rm -rf build test
