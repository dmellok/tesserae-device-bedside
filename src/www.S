/* Embed the web UI pages (www/index.html for phones, www/desktop.html for
 * desktops; both built by scripts/build_www.py) into flash via .incbin. */
.section .rodata
.balign 4
.global www_index
www_index:
.incbin "www/index.html"
.global www_index_end
www_index_end:
.balign 4
.global www_desktop
www_desktop:
.incbin "www/desktop.html"
.global www_desktop_end
www_desktop_end:
/* www sha1 18e7d3c48f58fd7bf6e0c8d51154f16de2715461 b7ed22dde29e02e829ed9215978a14bac2b38066 */
