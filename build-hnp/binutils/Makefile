all: download/binutils-2.44.tar.xz
	rm -rf temp build
	mkdir -p temp build
	cd temp && tar xvf ../download/binutils-2.44.tar.xz
	cd temp/binutils-2.44 && mkdir build
	cd temp/binutils-2.44/build && ../configure --disable-ld --without-zstd --host aarch64-unknown-linux-musl CC=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/aarch64-unknown-linux-ohos-clang AR=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-ar RANLIB=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-ranlib STRIP=$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-strip
	cd temp/binutils-2.44/build && make -j $(shell nproc) && make install DESTDIR=$(shell pwd)/build
	$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-strip build/usr/local/bin/*
	$(TOOL_HOME)/sdk/default/openharmony/native/llvm/bin/llvm-strip build/usr/local/aarch64-unknown-linux-musl/bin/*
	mkdir -p ../sysroot
	cp -rfv build/usr/local/. ../sysroot

download/binutils-2.44.tar.xz:
	mkdir -p download
	cd download && wget https://mirrors.tuna.tsinghua.edu.cn/gnu/binutils/binutils-2.44.tar.xz
