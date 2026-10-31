/* Embed the web UI page (www/index.html) into flash via .incbin. */
.section .rodata
.balign 4
.global www_index
www_index:
.incbin "www/index.html"
.global www_index_end
www_index_end:
/* www/index.html sha1 c628e88fe28c53ec06668f27ef5a2861bef4767a */
