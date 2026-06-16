# custom_memcmp/CMakeLists.txt

# Link the actual source file to this library
add_library(custom_memcmp STATIC memcmp_wrapper.c)