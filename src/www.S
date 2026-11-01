/* Embed the web UI page (www/index.html) into flash via .incbin. */
.section .rodata
.balign 4
.global www_index
www_index:
.incbin "www/index.html"
.global www_index_end
www_index_end:
/* www/index.html sha1 5a65c72f88c204e4267a9c57124998955cd7e453 */
