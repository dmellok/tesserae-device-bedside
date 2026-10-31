/* Embed the web UI page (www/index.html) into flash via .incbin. */
.section .rodata
.balign 4
.global www_index
www_index:
.incbin "www/index.html"
.global www_index_end
www_index_end:
/* www/index.html sha1 ea97ccd3d6d292f0d76bd3a2a7b17a8e159d2487 */
