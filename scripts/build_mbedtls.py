#
# build_mbedtls.py (PRE script): compile mbedTLS (the SDK's copy) and lwIP's
# altcp TLS layer, which the PlatformIO picosdk builder leaves out, for HTTPS
# to GitHub (over-the-air updates). Config: include/bedside_mbedtls_config.h.
#
from os.path import join
Import("env")
fw = env.PioPlatform().get_package_dir("framework-picosdk")
mbed = join(fw, "lib", "mbedtls")
env.Append(CPPPATH=[join(mbed, "include"),
                    join(fw, "src", "rp2_common", "pico_lwip"),                 # the patched altcp_tls_mbedtls.c
                    join(fw, "lib", "lwip", "src", "apps", "altcp_tls")])       # its _mem.c and structs header
env.Append(CPPDEFINES=[("MBEDTLS_CONFIG_FILE", '\\"bedside_mbedtls_config.h\\"')])
lib = env.BuildLibrary(join("$BUILD_DIR", "MbedTLS"), join(mbed, "library"), src_filter="+<*.c>")
env.Prepend(LIBS=[lib])
