# live555

LIVE555_VERSION := 2016.11.28
LIVE555_FILE := live.$(LIVE555_VERSION).tar.gz
LIVEDOTCOM_URL := http://live555.com/liveMedia/public/$(LIVE555_FILE)
LIVE555_FOLDER := live555

ifdef BUILD_NETWORK
ifdef GNUV3
PKGS += live555
endif
endif

ifeq ($(call need_pkg,"live555"),)
PKGS_FOUND += live555
endif

LIVE_EXTRA_CFLAGS := $(EXTRA_CFLAGS) -fexceptions $(CFLAGS)

LIVE_TARGET = $(error live555 target not defined!)
ifdef HAVE_LINUX
LIVE_TARGET := linux
LIVE_EXTRA_CFLAGS += -DXLOCALE_NOT_USED
endif
ifdef HAVE_WIN32
LIVE_TARGET := mingw
endif
ifdef HAVE_DARWIN_OS
LIVE_TARGET := macosx
else
ifdef HAVE_BSD
LIVE_TARGET := freebsd
endif
endif
ifdef HAVE_SOLARIS
ifeq ($(ARCH),x86_64)
LIVE_TARGET := solaris-64bit
else
LIVE_TARGET := solaris-32bit
endif
endif

$(TARBALLS)/$(LIVE555_FILE):
	$(call download_pkg,$(LIVEDOTCOM_URL),live555)

$(LIVE555_FOLDER):
	git clone https://github.com/niozhao/live555_2016.11.28.git $(LIVE555_FOLDER)
	# Change permissions to patch and sed the source
	chmod -R u+w $(LIVE555_FOLDER)
	# Remove hardcoded cc, c++, ar variables
	cd $(LIVE555_FOLDER) && sed -e 's%cc%$(CC)%' -e 's%c++%$(CXX)%' -e 's%LIBRARY_LINK =.*ar%LIBRARY_LINK = $(AR)%' -i.orig config.$(LIVE_TARGET)
	# Replace libtool -s by ar cr for macOS only
	cd $(LIVE555_FOLDER) && sed -i.orig -e s/"libtool -s -o"/"ar cr"/g config.macosx*
	# Add Extra LDFLAGS for macOS
	cd $(LIVE555_FOLDER) && sed -i.orig -e 's%$(CXX)%$(CXX)\ $(EXTRA_LDFLAGS)%' config.macosx
	# Add the Extra_CFLAGS to all config files
	cd $(LIVE555_FOLDER) && sed -i.orig \
		-e 's%^\(COMPILE_OPTS.*\)$$%\1 '"$(LIVE_EXTRA_CFLAGS)%" config.*
	# We want 64bits offsets and PIC on Linux
	cd $(LIVE555_FOLDER) && sed -e 's%-D_FILE_OFFSET_BITS=64%-D_FILE_OFFSET_BITS=64\ -fPIC\ -DPIC%' -i.orig config.linux
	# Disable Locale for Solaris
	cd $(LIVE555_FOLDER) && sed -e 's%-DSOLARIS%-DSOLARIS -DXLOCALE_NOT_USED%' -i.orig config.solaris-*bit
ifdef HAVE_ANDROID
	# Disable locale on Android too
	cd $(LIVE555_FOLDER) && sed -e 's%-DPIC%-DPIC -DNO_SSTREAM=1 -DLOCALE_NOT_USED -I$(ANDROID_NDK)/platforms/$(ANDROID_API)/arch-$(PLATFORM_SHORT_ARCH)/usr/include%' -i.orig config.linux
endif

.sum-live555: $(LIVE555_FILE) $(LIVE555_FOLDER)

SUBDIRS=groupsock liveMedia UsageEnvironment BasicUsageEnvironment

.live555: $(LIVE555_FOLDER) .sum-live555
	$(REQUIRE_GNUV3)
	cd $< && for subdir in $(SUBDIRS); do \
		echo "PREFIX = $(PREFIX)" >> $$subdir/Makefile.head && \
		echo "LIBDIR = $(PREFIX)/lib" >> $$subdir/Makefile.head ; done
	cd $< && echo "LIBDIR = $(PREFIX)/lib" >> Makefile.head && \
		echo "PREFIX = $(PREFIX)" >> Makefile.head
	cd $< && ./genMakefiles $(LIVE_TARGET)
	cd $< && for subdir in $(SUBDIRS); do $(MAKE) $(HOSTVARS) -C $$subdir; done
	cd $< && for subdir in $(SUBDIRS); do $(MAKE) $(HOSTVARS) -C $$subdir install; done
	cd $< && $(MAKE) install_shared_libraries
	touch $@

