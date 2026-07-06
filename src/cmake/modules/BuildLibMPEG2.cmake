macro( build_libmpeg2 )

include( CheckCCompilerFlag )

find_program( LIBTOOLIZE_EXECUTABLE
    NAMES glibtoolize libtoolize
    REQUIRED )

if( CMAKE_CROSSCOMPILING )
    string( REGEX MATCH "([-A-Za-z0-9\\._]+)-(gcc|cc)$" RESULT ${CMAKE_C_COMPILER} )
    string( REGEX REPLACE "-(gcc|cc)$" "" RESULT ${RESULT} )
    set( CONFIGURE_ARGS "--host=${RESULT}" )
endif()

check_c_compiler_flag(
    "-Wdeprecated-non-prototype"
    HAVE_DEPRECATED_NON_PROTOTYPE
)

set( LIBMPEG2_CFLAGS "-std=gnu89" )

if( HAVE_DEPRECATED_NON_PROTOTYPE )
    string( APPEND LIBMPEG2_CFLAGS " -Wno-deprecated-non-prototype" )
endif()

if( CMAKE_POSITION_INDEPENDENT_CODE )
    string( APPEND LIBMPEG2_CFLAGS " -fPIC" )
endif()

set( LIBMPEG2_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/3rdparty )

if( WIN32 )
    # On Windows/MSYS2, cmake's ExternalProject runs <SOURCE_DIR>/configure
    # with an absolute Windows path as $0, which breaks autoconf's srcdir
    # detection and ac_aux_dir lookup. Use a bash wrapper script that runs
    # ./configure from within the source directory to keep paths relative.
    set( LIBMPEG2_CONFIGURE_WRAPPER ${CMAKE_CURRENT_BINARY_DIR}/configure_libmpeg2.sh )
    file( WRITE ${LIBMPEG2_CONFIGURE_WRAPPER}
        "#!/bin/bash\n"
        "set -e\n"
        "libtoolize --copy --force\n"
        "autoreconf -f -i\n"
        "./configure ${CONFIGURE_ARGS} --quiet"
        " --prefix=\"$(cygpath -u '${LIBMPEG2_PREFIX}')\""
        " --disable-shared --enable-static --disable-sdl\n"
    )
    set( LIBMPEG2_CONFIGURE_CMD bash ${LIBMPEG2_CONFIGURE_WRAPPER} )
else()
    set( LIBMPEG2_CONFIGURE_CMD
        ${LIBTOOLIZE_EXECUTABLE} --copy --force &&
        autoreconf -f -i &&
        <SOURCE_DIR>/configure
        ${CONFIGURE_ARGS}
        --quiet
        --prefix=${LIBMPEG2_PREFIX}
        --disable-shared
        --enable-static
        --disable-sdl
    )
endif()

externalproject_add( libmpeg2
	PREFIX ${LIBMPEG2_PREFIX}
	URL ../../../src/3rdparty/libmpeg2/libmpeg2-master.tgz
	URL_HASH SHA256=5aad06f396553c5b6afb5393ff26187bb1120928d6ed4f88d2482dd41d04cf75

	CONFIGURE_COMMAND ${LIBMPEG2_CONFIGURE_CMD}

	BUILD_IN_SOURCE 1
	BUILD_COMMAND make V=0 CFLAGS=${LIBMPEG2_CFLAGS}
	INSTALL_DIR ${LIBMPEG2_PREFIX}
	INSTALL_COMMAND make LIBTOOLFLAGS=--silent install
	BUILD_BYPRODUCTS ${LIBMPEG2_PREFIX}/lib/libmpeg2.a
	${DOWNLOAD_ARGS}
)

set( MPEG2_INCLUDE_DIRS ${LIBMPEG2_PREFIX}/include/mpeg2dec )
set( MPEG2_LIBRARIES ${LIBMPEG2_PREFIX}/lib/libmpeg2.a )
set( MPEG2_FOUND ON )

endmacro( build_libmpeg2 )
