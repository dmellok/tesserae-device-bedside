#
# lwip_prepend.py (PRE script): make the project's include/lwipopts.h win.
#
# The picosdk framework's CYW43/lwIP build adds the bundled btstack lwIP "port"
# directory (which has its own lwipopts.h) to the include path. Without this,
# the framework compiles lwIP core against btstack's lwipopts (LWIP_DNS off, a
# smaller TCP window) instead of ours, and dns_gethostbyname() fails to link.
# Prepending the project include dir here (before the framework appends
# btstack's) ensures our lwipopts.h is found first by every translation unit.
# From tesserae-device-pico-bin.
#
Import("env")
env.Prepend(CPPPATH=[env["PROJECT_INCLUDE_DIR"]])
