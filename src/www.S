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
/* www sha1 55d56b438afffe2a637ee0391304d97937267582 900d423cb79af5e4f7bc69368f4e0086826a5099 */
