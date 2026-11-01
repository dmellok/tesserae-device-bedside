/* Embed the web UI page (www/index.html) into flash via .incbin. */
.section .rodata
.balign 4
.global www_index
www_index:
.incbin "www/index.html"
.global www_index_end
www_index_end:
/* www/index.html sha1 6c3ac4272c1d8ef8ef5f3814b69cba175389c585 */
