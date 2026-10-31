/* The dawn-chorus clips (assets/birds/birds.ulaw, 8-bit mu-law, 22.05 kHz). */
.section .rodata
.balign 4
.global bird_data
bird_data:
.incbin "assets/birds/birds.ulaw"
.global bird_data_end
bird_data_end:
/* sha1 99b23cfe855c2b6051e88341bb75bbf419e93dd8 */
