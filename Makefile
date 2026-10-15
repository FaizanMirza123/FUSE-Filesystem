CXXFLAGS ?= -std=c++17 -g -Wall -Wextra

# FUSE 2.x: libfuse on Linux / macFUSE register as "fuse", FUSE-T as "fuse-t".
FUSE_PKG    := $(shell pkg-config --exists fuse 2>/dev/null && echo fuse || \
                 (pkg-config --exists fuse-t 2>/dev/null && echo fuse-t))
FUSE_CFLAGS := $(if $(FUSE_PKG),$(shell pkg-config --cflags $(FUSE_PKG)))
FUSE_LIBS   := $(if $(FUSE_PKG),$(shell pkg-config --libs $(FUSE_PKG)))

ifeq ($(shell uname),Darwin)
UNMOUNT := umount
else
UNMOUNT := fusermount -u
endif

.PHONY: all check-fuse mount unmount clean

all: bbfs

check-fuse:
	@test -n "$(FUSE_PKG)" || { \
	  echo "FUSE 2.x development files not found (pkg-config fuse / fuse-t)."; \
	  echo "  Debian/Ubuntu: sudo apt install libfuse-dev fuse"; \
	  echo "  Fedora:        sudo dnf install fuse-devel"; \
	  echo "  macOS:         brew install --cask macfuse   (or fuse-t)"; \
	  exit 1; }

bbfs: check-fuse bbfs.o log.o
	$(CXX) $(CXXFLAGS) -o $@ bbfs.o log.o $(FUSE_LIBS)

bbfs.o: bbfs.cpp log.h params.h | check-fuse
	$(CXX) $(CXXFLAGS) $(FUSE_CFLAGS) -c bbfs.cpp

log.o: log.cpp log.h params.h | check-fuse
	$(CXX) $(CXXFLAGS) $(FUSE_CFLAGS) -c log.cpp

# Mount example/rootdir on example/mountdir; the log goes to example/bbfs.log.
mount: bbfs
	mkdir -p example/mountdir
	cd example && ../bbfs rootdir mountdir

unmount:
	$(UNMOUNT) example/mountdir

clean:
	rm -f bbfs *.o example/bbfs.log
