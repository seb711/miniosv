include(ExternalProject)
find_package(Git REQUIRED)

# Build Boost.Context from source
ExternalProject_Add(
    boost_src
    PREFIX "vendor/boost"
    URL "https://archives.boost.io/release/1.81.0/source/boost_1_81_0.tar.bz2"
    URL_HASH SHA256=71feeed900fbccca04a3b4f2f84a7c217186f28a940ed8b7ed4725986baf99fa
    DOWNLOAD_NO_PROGRESS FALSE
    TIMEOUT 600
    CONFIGURE_COMMAND <SOURCE_DIR>/bootstrap.sh 
        --prefix=<INSTALL_DIR>
        --with-libraries=context
    BUILD_COMMAND <SOURCE_DIR>/b2 
        --prefix=<INSTALL_DIR>
        --build-dir=<BINARY_DIR>
        link=static
        threading=multi
        variant=release
        cxxflags=-fPIC
        install
    BUILD_IN_SOURCE 0
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
)

# Prepare Boost
ExternalProject_Get_Property(boost_src install_dir)
ExternalProject_Get_Property(boost_src binary_dir)
set(Boost_INCLUDE_DIR ${install_dir}/include)
set(Boost_LIBRARY_DIR ${binary_dir}/stage/lib)
set(Boost_CONTEXT_LIBRARY ${Boost_LIBRARY_DIR}/libboost_context.a)
file(MAKE_DIRECTORY ${Boost_INCLUDE_DIR})

# Create imported target
add_library(Boost::context STATIC IMPORTED)
set_property(TARGET Boost::context PROPERTY IMPORTED_LOCATION ${Boost_CONTEXT_LIBRARY})
set_property(TARGET Boost::context APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${Boost_INCLUDE_DIR})

# Compatibility variables
set(Boost_FOUND TRUE)
set(Boost_LIBRARIES Boost::context)
set(Boost_INCLUDE_DIRS ${Boost_INCLUDE_DIR})

# Dependencies
add_dependencies(Boost::context boost_src)