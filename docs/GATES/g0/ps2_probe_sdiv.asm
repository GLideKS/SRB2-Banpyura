
D:\Ai-Project3\SRB2-PS2-Port\build\ps2-hello\HELLO.ELF:     file format elf32-nlittlemips


Disassembly of section .text:

00105100 <ps2_probe_sdiv>:
  105100:	27bdfff0 	addiu	sp,sp,-16
  105104:	ffbf0008 	sd	ra,8(sp)
  105108:	0c041daa 	jal	1076a8 <__divdi3>
  10510c:	00000000 	nop
  105110:	dfbf0008 	ld	ra,8(sp)
  105114:	03e00008 	jr	ra
  105118:	27bd0010 	addiu	sp,sp,16
