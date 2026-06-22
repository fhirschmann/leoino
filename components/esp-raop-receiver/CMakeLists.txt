idf_component_register(
    SRCS
        "src/raop_core.c"
        "src/raop.c"
        "src/rtp.c"
        "src/util.c"
        "src/dmap_parser.c"
        "src/audio_buffer.c"
        # Apple ALAC decoder (Apache-2.0) + philippe44-style wrapper, compiled from
        # source instead of the upstream prebuilt libalac.a (nothing opaque is linked).
        "alac/alac_wrapper.cpp"
        "alac/ALACDecoder.cpp"
        "alac/ALACBitUtilities.c"
        "alac/EndianPortable.c"
        "alac/ag_dec.c"
        "alac/dp_dec.c"
        "alac/matrix_dec.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
        "private_include"
        "alac"
    REQUIRES
        esp_netif
        mdns
        mbedtls
        nvs_flash
    PRIV_REQUIRES
        esp_timer
        lwip
        pthread
)

# Apple's EndianPortable.c only auto-detects little-endian for x86; the ESP32 (xtensa)
# and ESP32-S3 are little-endian, so force it here or the byte-swaps invert.
target_compile_definitions(${COMPONENT_LIB} PRIVATE TARGET_RT_LITTLE_ENDIAN=1)

# The vendored ALAC reference source predates -Wall/-Wextra cleanliness; keep its
# warnings from failing the build without relaxing flags for the rest of the firmware.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
