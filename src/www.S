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
/* www sha1 6becab4a9317d62ee9e8d4c23f1a452b0c4c388f 80633cb19518d3249e6c87cc370dd0278aecf4c5 */
