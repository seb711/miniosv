.PHONY: module clean

# os-features is a plain Linux PIE built with the host C++ toolchain, exactly
# like apps/native-example's hello.c - OSv loads and runs it. It only touches
# the standard threads/mmap/time/libc surface, so no OSv headers are needed.

CXX ?= g++
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wextra -fPIE
LDFLAGS = -pie -rdynamic -pthread

module: os-features

os-features: os-features.cc
	$(CXX) $(CXXFLAGS) -o $@ os-features.cc $(LDFLAGS)

clean:
	rm -f os-features
