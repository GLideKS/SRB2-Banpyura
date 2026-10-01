
D:\Ai-Project3\SRB2-PS2-Port\build\ps2-hello\HELLO.ELF:     file format elf32-nlittlemips


Disassembly of section .text:

00105120 <ps2_probe_udiv>:
  105120:	27bdfff0 	addiu	sp,sp,-16
  105124:	ffbf0008 	sd	ra,8(sp)
  105128:	0c04227c 	jal	1089f0 <__udivdi3>
  10512c:	00000000 	nop
  105130:	dfbf0008 	ld	ra,8(sp)
  105134:	03e00008 	jr	ra
  105138:	27bd0010 	addiu	sp,sp,16
