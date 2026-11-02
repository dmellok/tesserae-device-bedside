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
/* www sha1 1b404bf636c0ffc67ef0a740e7b3f405d8ed8e77 5e9417dc5fadf10074f1fb9b0ae7432732b85ab4 */
