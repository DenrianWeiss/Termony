PKGS=bash \
	binutils \
	busybox \
	coremark \
	lz4 \
	make \
	ncurses \
	openssl \
	strace \
	tar \
	tree \
	vim \
	xxhash \
	libunistring \
	libidn2 \
	c-ares \
	curl \
	yyjson \
	fastfetch \
	sl \
	openssh \
	ncnn \
	readline \
	htop \
	vkpeak \
	kbd \
	stream
STAMP=$(patsubst %,%/.stamp,$(PKGS))

all: copy

copy: base.hnp
	rm -f ../entry/hnp/arm64-v8a/*.hnp
	cp $^ ../entry/hnp/arm64-v8a

base.hnp: $(STAMP)
	$(TOOL_HOME)/sdk/default/openharmony/toolchains/hnpcli pack -i sysroot -n base -v 1.0

%/.stamp: %/Makefile
	make -C $(patsubst %/.stamp,%,$@)
	touch $@

clean:
	rm -f $(STAMP)
	rm -rf sysroot
