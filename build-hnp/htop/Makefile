all: download/htop-3.4.1.tar.xz
	rm -rf temp build
	mkdir -p temp build/bin
	cd temp && tar xvf ../download/htop-3.4.1.tar.xz
	cd temp/htop-3.4.1 && cat ../../0001-skip-proc-stat.diff | patch -Np1
	cd temp/htop-3.4.1 && PKG_CONFIG=/usr/bin/false ./configure --host aarch64-unknown-linux-musl --enable-affinity --enable-static CC=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/aarch64-unknown-linux-ohos-clang AR=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-ar RANLIB=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-ranlib STRIP=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-strip CPPFLAGS="-I$(shell pwd)/../sysroot/include -D_FORTIFY_SOURCE=2" LDFLAGS="-L$(shell pwd)/../sysroot/lib"
	cd temp/htop-3.4.1 && make -j $(shell nproc) && make install DESTDIR=$(shell pwd)/build
	mkdir -p ../sysroot
	cp -rv ./build/usr/local/. ../sysroot

download/htop-3.4.1.tar.xz:
	mkdir -p download
	cd download && wget https://github.com/htop-dev/htop/releases/download/3.4.1/htop-3.4.1.tar.xz
