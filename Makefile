# vigil Makefile

CC?=cc
AR?=ar
OBJDIR?=obj
VERSION=$(OBJDIR)/version

LIB=libvigil.a

PLATFORM?=host
KYRKA?=build/host
FLUTTER?=
LIBSODIUM_PATH?=$(KYRKA)/libsodium

CFLAGS+=-std=c99 -pedantic -Wall -Werror -Wstrict-prototypes
CFLAGS+=-Wmissing-prototypes -Wmissing-declarations -Wshadow
CFLAGS+=-Wpointer-arith -Wcast-qual -Wsign-compare -O2 -fPIC
CFLAGS+=-fstack-protector-all -Wtype-limits -fno-common -g
CFLAGS+=-Iinclude -I$(KYRKA)/include

LDFLAGS+=$(KYRKA)/lib/libkyrka.a

CFLAGS+=-I$(LIBSODIUM_PATH)/include
LDFLAGS+=$(LIBSODIUM_PATH)/lib/libsodium.a

CFLAGS+=-I$(KYRKA)/opus/include
LDFLAGS+=$(KYRKA)/opus/lib/libopus.a -lm

SRC=	src/vigil.c \
	src/buf.c \
	src/call.c \
	src/chat.c \
	src/config.c \
	src/ether.c \
	src/grace.c \
	src/inet.c \
	src/io.c \
	src/liturgy.c \
	src/peer.c \
	src/remembrance.c \
	src/ring.c \
	src/status.c \
	src/video.c \
	src/voice.c

ifeq ("$(SANITIZE)", "1")
	CFLAGS+=-fsanitize=address,undefined
	LDFLAGS+=-fsanitize=address,undefined
endif

ifeq ("$(PLATFORM)", "host")
	SRC+=src/platform_host.c src/audio_host.c src/video_host.c
	CFLAGS+=-DPLATFORM_HOST
	CFLAGS+=-D_GNU_SOURCE=1 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
else ifeq ("$(PLATFORM)", "ios")
	SRC+=src/platform_ios.c src/audio_ios.c src/video_ios.c
	CFLAGS+=-DPLATFORM_IOS
else
$(error unknown platform $(PLATFORM), android is built via app/android)
endif

TESTS=	tests/config_test \
	tests/inet_test \
	tests/tunnel_test \
	tests/grace_test \
	tests/video_test

all:
ifeq ("$(PLATFORM)", "host")
	$(MAKE) $(KYRKA)/lib/libkyrka.a
endif
	$(MAKE) $(OBJDIR)
	$(MAKE) $(LIB)

OBJS=	$(SRC:%.c=$(OBJDIR)/%.o)
OBJS+=	$(OBJDIR)/version.o

$(LIB): $(OBJS) $(VERSION).c
	$(AR) rcs $(LIB) $(OBJS)

$(OBJDIR):
	@mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: %.c include/vigil.h
	@mkdir -p $(shell dirname $@)
	$(CC) $(CFLAGS) -c $< -o $@

src/vigil.c: $(VERSION).c

version: $(VERSION).c

$(VERSION).c: force
	@mkdir -p $(OBJDIR)
	@if [ -f RELEASE ]; then \
		printf "const char *vigil_build_rev = \"%s\";\n" \
		    `cat RELEASE` > $(VERSION)_gen; \
	elif [ -d .git ]; then \
		GIT_REVISION=`git rev-parse --short=8 HEAD`; \
		GIT_BRANCH=`git rev-parse --abbrev-ref HEAD`; \
		rm -f $(VERSION)_gen; \
		printf "const char *vigil_build_rev = \"%s-%s\";\n" \
		    $$GIT_BRANCH $$GIT_REVISION > $(VERSION)_gen; \
	else \
		echo "No version information found (no .git or RELEASE)"; \
		exit 1; \
	fi
	@printf "const char *vigil_build_date = \"%s\";\n" \
	    `date +"%Y-%m-%d"` >> $(VERSION)_gen;
	@if [ -f $(VERSION).c ]; then \
		cmp -s $(VERSION)_gen $(VERSION).c; \
		if [ $$? -ne 0 ]; then \
			cp $(VERSION)_gen $(VERSION).c; \
		fi \
	else \
		cp $(VERSION)_gen $(VERSION).c; \
	fi

$(KYRKA)/lib/libkyrka.a: $(wildcard dist-build/patches/libkyrka-*.patch)
	./dist-build/host-build.sh

tests: $(KYRKA)/lib/libkyrka.a
	$(MAKE) $(OBJDIR)
	$(MAKE) $(LIB)
	$(MAKE) $(TESTS) tests/pong tests/vigild tests/caller tests/chatter

tests-run: tests
	@for t in $(TESTS); do \
		echo "running $$t"; \
		./$$t || exit 1; \
	done

tests/%: tests/%.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -lpthread -o $@

deps:
	./dist-build/deps.sh

android: android-deps
	FLUTTER=$(FLUTTER) ./dist-build/android-app.sh --release
	@echo "built app/build/app/outputs/flutter-apk/app-release.apk"

ios: ios-deps
	FLUTTER=$(FLUTTER) ./dist-build/ios-app.sh --release $(IOS_FLAGS)
	@echo "built app/build/ios/iphoneos/Runner.app"

android-run: android-deps
	FLUTTER=$(FLUTTER) ./dist-build/android-run.sh $(RUN_FLAGS)

android-deps:
	./dist-build/android-build.sh

ios-deps:
	./dist-build/ios-build.sh

clean: clean-objs
	find build -mindepth 1 -maxdepth 1 ! -name dl -exec rm -rf {} + \
	    2> /dev/null || true
	rm -rf app/build app/.dart_tool app/android/app/.cxx
	rm -rf app/android/.gradle app/ios/Flutter/ephemeral

clean-objs:
	rm -f $(VERSION)
	rm -rf $(OBJDIR) $(LIB) $(TESTS) tests/pong tests/vigild tests/caller \
	    tests/chatter

.PHONY: all clean clean-objs force deps tests tests-run version
.PHONY: android android-deps android-run ios ios-deps
