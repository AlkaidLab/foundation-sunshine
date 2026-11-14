# Boost.Asio and Boost.Beast, compiled once.
#
# Both are header-only by default, so every translation unit that includes them parses and compiles the same non-template
# function bodies. Boost.Asio documents the alternative (Using Boost.Asio, "Optional separate compilation"): include
# boost/asio/impl/src.hpp in exactly one source file and define BOOST_ASIO_SEPARATE_COMPILATION for the whole program;
# Boost.Beast has the same switch (BOOST_BEAST_SEPARATE_COMPILATION, boost/beast/src.hpp). Measured on the host sources it
# takes about 8% off the compile time (24 representative translation units, three interleaved repeats on one machine).
#
# The definitions are directory-wide and the library is linked into every target created afterwards, so no target can end up
# on the other side of the switch.
option(SUNSHINE_BOOST_SEPARATE_COMPILATION "Compile Boost.Asio and Boost.Beast once instead of in every translation unit" ON)
if(SUNSHINE_BOOST_SEPARATE_COMPILATION AND TARGET Boost::asio AND TARGET Boost::beast AND TARGET OpenSSL::SSL)
    add_compile_definitions(BOOST_ASIO_SEPARATE_COMPILATION BOOST_BEAST_SEPARATE_COMPILATION)
    add_library(sunshine_boost_separate STATIC "${CMAKE_SOURCE_DIR}/src/boost_separate.cpp")
    set_target_properties(sunshine_boost_separate PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(sunshine_boost_separate PUBLIC Boost::asio Boost::beast OpenSSL::SSL OpenSSL::Crypto)
    if(WIN32)
        # The separately compiled implementation needs its system libraries wherever it is linked, and it is linked into every
        # target: BCryptGenRandom (random bytes for websocket masking and TLS seeding) comes from bcrypt, the static OpenSSL
        # in it needs crypt32.
        target_link_libraries(sunshine_boost_separate PUBLIC ws2_32 mswsock bcrypt crypt32)
    endif()
    link_libraries(sunshine_boost_separate)
endif()
