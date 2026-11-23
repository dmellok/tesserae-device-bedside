/* The dawn-chorus clips, embedded as src/birds_data.S does on the device
 * (Mach-O spelling: leading underscores, __DATA,__const). */
.section __DATA,__const
.p2align 2
.globl _bird_data
_bird_data:
.incbin "assets/birds/birds.ulaw"
.globl _bird_data_end
_bird_data_end:
