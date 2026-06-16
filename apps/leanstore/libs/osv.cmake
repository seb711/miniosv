message(STATUS "=== Starting osv.cmake ===")

if(NOT DEFINED ENV{OSV_BASE})
    message(FATAL_ERROR "OSV_BASE environment variable not set")
endif()

set(OSV_BASE $ENV{OSV_BASE})
message(STATUS "OSV_BASE: ${OSV_BASE}")

# Determine architecture
if(NOT DEFINED ARCH)
    if(DEFINED ENV{ARCH})
        set(ARCH $ENV{ARCH})
    else()
        set(ARCH "x64")
    endif()
endif()
message(STATUS "ARCH: ${ARCH}")

# Check if target already exists
if(TARGET osv)
    message(STATUS "OSV target already exists, skipping")
    return()
endif()

# Create OSV interface library
add_library(osv INTERFACE)

# CRITICAL: Add OSV includes as SYSTEM includes
# This gives them LOWER priority than standard library headers
# which resolves the mutex ambiguity
target_include_directories(osv SYSTEM INTERFACE
    ${OSV_BASE}/arch/${ARCH}
    ${OSV_BASE}
    ${OSV_BASE}/include
    ${OSV_BASE}/arch/common
    ${OSV_BASE}/build/release.x64/gen/include
)

# You don't need to manually add the C++ standard library paths
# CMake handles this automatically, and marking OSV as SYSTEM
# ensures standard library is searched first

# Find Boost
find_package(Boost COMPONENTS system filesystem)
if(Boost_FOUND)
    message(STATUS "✓ Boost found: ${Boost_VERSION}")
    target_link_libraries(osv INTERFACE Boost::system Boost::filesystem)
else()
    message(WARNING "Boost not found")
endif()

# Architecture-specific definitions
if(ARCH STREQUAL "aarch64")
    target_compile_definitions(osv INTERFACE AARCH64_PORT_STUB)
endif()

# Add compile options for OSV compatibility
target_compile_options(osv INTERFACE -fPIC)

message(STATUS "=== OSV configuration complete ===")

# Debug output
get_target_property(OSV_INCLUDES osv INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(OSV_SYSTEM_INCLUDES osv INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
message(STATUS "Final OSV includes: ${OSV_INCLUDES}")
message(STATUS "Final OSV system includes: ${OSV_SYSTEM_INCLUDES}")