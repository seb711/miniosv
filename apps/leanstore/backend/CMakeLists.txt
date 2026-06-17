# ---------------------------------------------------------------------------
# Leanstore
# ---------------------------------------------------------------------------
project(leanstore)

# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------
file(GLOB_RECURSE LEANSTORE_CC **.cpp **/**.cpp **.hpp **/**.hpp)
if (NOT UNIX)
    message(SEND_ERROR "unsupported platform")
endif ()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(leanstore SHARED ${LEANSTORE_CC})

# ---------------------------------------------------------------------------
# Helper function to find static library or fall back to dynamic
# ---------------------------------------------------------------------------
function(target_link_library_prefer_static target lib)
    # Try to find the static version first
    find_library(${lib}_STATIC_LIB lib${lib}.a)
    
    if(${lib}_STATIC_LIB)
        message(STATUS "Linking ${lib} statically: ${${lib}_STATIC_LIB}")
        target_link_libraries(${target} ${${lib}_STATIC_LIB})
    else()
        message(STATUS "Linking ${lib} dynamically (static version not found)")
        target_link_libraries(${target} ${lib})
    endif()
    
    unset(${lib}_STATIC_LIB CACHE)
endfunction()

# ---------------------------------------------------------------------------
# Link libraries - prefer static except osv
# ---------------------------------------------------------------------------

# Try to link these statically if available, otherwise dynamic
target_link_library_prefer_static(leanstore gflags)
target_link_libraries(leanstore Threads::Threads)
# target_link_library_prefer_static(leanstore aio)
target_link_library_prefer_static(leanstore atomic)
target_link_library_prefer_static(leanstore tabluate)
target_link_library_prefer_static(leanstore custom_memcmp)

# Always link osv dynamically
target_link_libraries(leanstore osv fakeosv aio)

target_link_options(leanstore PRIVATE -Wl,-wrap,memcmp -Wl,-wrap,memcpy -Wl,-wrap,memmove)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
add_dependencies(leanstore liburing)


# xnvme stuff
if(LEANSTORE_INCLUDE_XNVME)
   add_dependencies(leanstore xnvme)
   message("Including XNVME")
   
   # Try to find static versions
   find_library(XNVME_STATIC_LIB libxnvme.a PATHS ${X_LIBRARY_PATH})
   find_library(SSL_STATIC_LIB libssl.a)
   
   if(XNVME_STATIC_LIB)
       target_link_libraries(leanstore 
          -L${XNVME_LIBRARY_PATH}
          -Wl,--whole-archive -Wl,--no-as-needed
          ${XNVME_STATIC_LIB}
       )
   else()
       target_link_libraries(leanstore -L${XNVME_LIBRARY_PATH} -lxnvme)
   endif()
   
   if(SSL_STATIC_LIB)
       target_link_libraries(leanstore ${SSL_STATIC_LIB})
   else()
       target_link_libraries(leanstore -lssl)
   endif()
   
   target_link_libraries(leanstore
       -Wl,--no-whole-archive -Wl,--as-needed
       -pthread -lrt -lm -lnuma -luuid -laio
   )
   
   unset(XNVME_STATIC_LIB CACHE)
   unset(SSL_STATIC_LIB CACHE)
endif()

if(LEANSTORE_INCLUDE_SPDK)
    add_dependencies(leanstore spdk)
    # all the SPDK stuff
    target_link_libraries(leanstore -L${SPDK_LIBRARY_PATH})
    target_link_libraries(leanstore  -lspdk_vmd -Wl,--whole-archive -lspdk_nvme -Wl,--no-whole-archive -lspdk_trace -lspdk_sock -lspdk_log -lspdk_json -lspdk_env_dpdk -lspdk_util -lspdk_vfio_user)
    target_link_libraries(leanstore
        -Wl,--whole-archive
        ${DPDK_LIBRARY_PATH}/librte_eal.a
        ${DPDK_LIBRARY_PATH}/librte_telemetry.a
        ${DPDK_LIBRARY_PATH}/librte_mempool.a
        ${DPDK_LIBRARY_PATH}/librte_kvargs.a
        ${DPDK_LIBRARY_PATH}/librte_bus_pci.a
        ${DPDK_LIBRARY_PATH}/librte_pci.a
        ${DPDK_LIBRARY_PATH}/librte_ring.a
        -Wl,--no-whole-archive
    )
    target_link_libraries(leanstore -luuid -lnuma -lrt -lcrypto -laio)
    target_link_libraries(leanstore -L${ISAL_LIBRARY_PATH} -lisal )
endif()

# Boost context - static linking
set(Boost_USE_STATIC_LIBS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Try to find Boost, but handle Nix path issues gracefully
set(Boost_NO_SYSTEM_PATHS OFF)
find_package(Boost COMPONENTS context)

if(Boost_FOUND AND TARGET Boost::context)
    # Check if the include directories actually exist
    get_target_property(BOOST_INC_DIRS Boost::context INTERFACE_INCLUDE_DIRECTORIES)
    set(BOOST_INC_EXISTS TRUE)
    foreach(inc_dir ${BOOST_INC_DIRS})
        if(NOT EXISTS "${inc_dir}")
            message(WARNING "Boost include directory does not exist: ${inc_dir}")
            set(BOOST_INC_EXISTS FALSE)
        endif()
    endforeach()
    
    if(BOOST_INC_EXISTS)
        message(STATUS "Using system Boost")
        target_link_libraries(leanstore Boost::context)
    else()
        message(WARNING "System Boost has invalid paths, falling back to manual linking")
        set(Boost_FOUND FALSE)
    endif()
endif()

if(NOT Boost_FOUND)
    # Fallback: try to find Boost libraries manually
    find_library(BOOST_CONTEXT_LIB 
        NAMES libboost_context.a boost_context libboost_context.so
        HINTS ENV BOOST_ROOT
        PATH_SUFFIXES lib lib64
    )
    find_path(BOOST_INCLUDE_PATH boost/context/fiber.hpp
        HINTS ENV BOOST_ROOT
        PATH_SUFFIXES include
    )
    
    if(BOOST_CONTEXT_LIB AND BOOST_INCLUDE_PATH)
        message(STATUS "Found Boost manually: ${BOOST_CONTEXT_LIB}")
        target_link_libraries(leanstore ${BOOST_CONTEXT_LIB})
        target_include_directories(leanstore PRIVATE ${BOOST_INCLUDE_PATH})
    else()
        message(FATAL_ERROR "Boost context library not found. Please set BOOST_ROOT or install Boost.")
    endif()
endif()

# liburing - prefer static
find_library(URING_STATIC_LIB liburing.a PATHS ${liburing_LIBRARY_PATH})
if(URING_STATIC_LIB)
    message(STATUS "Linking liburing statically: ${URING_STATIC_LIB}")
    target_link_libraries(leanstore ${URING_STATIC_LIB})
else()
    message(STATUS "Linking liburing dynamically")
    target_link_libraries(leanstore -L${liburing_LIBRARY_PATH} -luring)
endif()
unset(URING_STATIC_LIB CACHE)

target_link_libraries(leanstore ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# Debugging options
# ---------------------------------------------------------------------------
OPTION(LEANSTORE_COUNTERS "Compile leanstore with debugging/performance counters" ON)
IF(LEANSTORE_COUNTERS)
  target_compile_definitions(leanstore PUBLIC MACRO_FLAG_COUNTERS=1)
ENDIF(LEANSTORE_COUNTERS)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(leanstore PUBLIC ${SHARED_INCLUDE_DIRECTORY})
target_include_directories(leanstore PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# ---------------------------------------------------------------------------
set(LEANSTORE_INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR})
set_property(TARGET leanstore APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${LEANSTORE_INCLUDE_DIR})