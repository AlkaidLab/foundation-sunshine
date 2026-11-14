cmake_minimum_required(VERSION 3.20)

# Source drop of the Platform core/network/stun primitive: a plain-C static library
# with no dependency on the Platform Runtime or Module SDK. OpenSSL provides HMAC and
# the digests used by MESSAGE-INTEGRITY; the host links OpenSSL already.
add_library(alkaidlab_stun_codec STATIC
        src/stun_codec.c)
target_include_directories(alkaidlab_stun_codec PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(alkaidlab_stun_codec PUBLIC OpenSSL::Crypto)
target_compile_features(alkaidlab_stun_codec PUBLIC c_std_11)
set_target_properties(alkaidlab_stun_codec PROPERTIES C_EXTENSIONS OFF)

if(BUILD_TESTS)
  add_executable(alkaidlab_stun_codec_test tests/stun_codec_test.c)
  target_link_libraries(alkaidlab_stun_codec_test PRIVATE alkaidlab_stun_codec OpenSSL::Crypto)
  add_test(NAME alkaidlab_stun_codec_test COMMAND alkaidlab_stun_codec_test)
endif()
