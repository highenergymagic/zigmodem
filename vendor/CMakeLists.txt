# Pinned portable subset; see SOURCES.txt. Unsupported algorithms fail closed.
set(N ${CMAKE_CURRENT_SOURCE_DIR}/noise)
add_library(zm_noise STATIC
  ${N}/src/protocol/cipherstate.c ${N}/src/protocol/dhstate.c
  ${N}/src/protocol/errors.c ${N}/src/protocol/handshakestate.c
  ${N}/src/protocol/hashstate.c ${N}/src/protocol/internal.c
  ${N}/src/protocol/names.c ${N}/src/protocol/patterns.c
  ${N}/src/protocol/symmetricstate.c ${N}/src/protocol/util.c
  ${N}/src/backend/ref/cipher-chachapoly.c
  ${N}/src/backend/ref/dh-curve25519.c ${N}/src/backend/ref/hash-blake2s.c
  ${N}/src/crypto/blake2/blake2s.c ${N}/src/crypto/chacha/chacha.c
  ${N}/src/crypto/donna/poly1305-donna.c ${N}/src/crypto/sha2/sha256.c
  noise_platform.c)
target_include_directories(zm_noise PUBLIC ${N}/include PRIVATE ${N}/src ${N}/src/protocol)
if(DEFINED ZEPHYR_BASE)
  target_link_libraries(zm_noise PUBLIC zephyr_interface)
else()
  target_sources(zm_noise PRIVATE ${N}/src/protocol/rand_os.c)
endif()
add_library(zm_heatshrink STATIC heatshrink/heatshrink_encoder.c heatshrink/heatshrink_decoder.c)
target_include_directories(zm_heatshrink PUBLIC heatshrink)
target_compile_definitions(zm_heatshrink PUBLIC HEATSHRINK_DYNAMIC_ALLOC=0)
if(DEFINED ZEPHYR_BASE)
  target_link_libraries(zm_heatshrink PUBLIC zephyr_interface)
endif()
