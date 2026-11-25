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
/* www sha1 4f05dc78c47a79fae4c0903d2beddab592e2e2da eb43d8764e3d919fd1aed818e22e92c18ff90b3b */
