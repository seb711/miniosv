.PHONY: module
BUILD_DIR = build
MEAN_TYPE ?= MEAN_USE_JOBBING # Default value, can be overridden
IS_OSV ?= 1 # Default to Linux, can be overridden with IS_LINUX=0
IS_ADAPTIVE ?= 0
IS_PERIODIC ?= 0
IS_WATCHDOG ?= 0

module: install-dependencies build-shared cmake-configure

LIBFAKEOSVDIR=$(OSV_BASE)/libfakeosv
LIB_SHARED = $(LIBFAKEOSVDIR)/libfakeosv.so

.PHONY: build-shared
build-shared:
	$(MAKE) -C $(LIBFAKEOSVDIR)

# Install required dependencies using apt-get
.PHONY: install-dependencies
install-dependencies:
	sudo apt-get update
	sudo apt-get install -y cmake libaio-dev libsnappy-dev zlib1g-dev \
		libbz2-dev liblz4-dev libzstd-dev librocksdb-dev liblmdb-dev \
		libwiredtiger-dev liburing-dev

# Configure compiler flags based on IS_LINUX
ifeq ($(IS_OSV),1)
    PLATFORM_FLAGS = -DIS_OSV=1
else
    PLATFORM_FLAGS = -DIS_OSV=0
endif

# Configure compiler flags based on IS_LINUX
ifeq ($(IS_ADAPTIVE),1)
    PLATFORM_FLAGS += -DIS_ADAPTIVE=1
else
    PLATFORM_FLAGS += -DIS_ADAPTIVE=0
endif

# Configure compiler flags based on IS_LINUX
ifeq ($(IS_PERIODIC),1)
    PLATFORM_FLAGS += -DIS_PERIODIC=1
else
    PLATFORM_FLAGS += -DIS_PERIODIC=0
endif

# Configure compiler flags based on IS_LINUt
ifeq ($(IS_WATCHDOG),1)
    PLATFORM_FLAGS += -DIS_WATCHDOG=1
else
    PLATFORM_FLAGS += -DIS_WATCHDOG=0
endif

# Configure the project with CMake, specifying GCC 12 as the compiler, linking against libtbb, and adding -fPIC for shared lib
.PHONY: cmake-configure
cmake-configure:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
		-DCMAKE_C_FLAGS="-fPIC" \
		-DCMAKE_CXX_FLAGS="-fPIC -DNOMUTEX -D$(MEAN_TYPE)" \
		 $(PLATFORM_FLAGS) \
		-DLIBFAKEOSV_PATH=$(LIB_SHARED) \
		 .. && make -j

# Clean the build directory
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

# Help target to show usage
.PHONY: help
help:
	@echo "Usage:"
	@echo "  make module                    # Build with default settings (Linux)"
	@echo "  make module IS_LINUX=0        # Build without Linux flag"
	@echo "  make module MEAN_TYPE=CUSTOM  # Override mean type"
	@echo "  make clean                     # Clean build directory"