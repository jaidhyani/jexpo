# Build against the Hyprland headers pkg-config finds (your distro's hyprland-dev / headers
# package, or hyprpm's). On Debian/Ubuntu without root, ./build fetches them for you instead.
CXX ?= g++
ifeq ($(CXX),g++)
    EXTRA_FLAGS = --no-gnu-unique
endif
SRC = main.cpp expo.cpp renumber.cpp

all: jexpo.so

jexpo.so: $(SRC) expo.hpp
	$(CXX) -shared -fPIC $(EXTRA_FLAGS) -O2 -g -std=c++2b -Wno-narrowing $(SRC) -o $@ \
		`pkg-config --cflags pixman-1 libdrm hyprland cairo libinput libudev wayland-server xkbcommon`

clean:
	rm -f jexpo.so

.PHONY: all clean
