add_library(ycsb SHARED ycsb/ycsb.cpp)
target_link_libraries(ycsb leanstore Threads::Threads custom_memcmp osv)
target_link_options(ycsb PRIVATE -Wl,-wrap,memcmp  -Wl,-wrap,memcpy -Wl,-wrap,memmove)
target_include_directories(ycsb PRIVATE ${SHARED_INCLUDE_DIRECTORY})

add_library(mixed SHARED mixed/mixed.cpp)
target_link_libraries(mixed leanstore Threads::Threads custom_memcmp osv)
target_link_options(mixed PRIVATE -Wl,-wrap,memcmp  -Wl,-wrap,memcpy -Wl,-wrap,memmove)
target_include_directories(mixed PRIVATE ${SHARED_INCLUDE_DIRECTORY})
