/* Embed the web UI page (www/index.html) into flash via .incbin. */
.section .rodata
.balign 4
.global www_index
www_index:
.incbin "www/index.html"
.global www_index_end
www_index_end:
/* www/index.html sha1 d7dde090433848be2091f0e4d03c587c677c119c */
