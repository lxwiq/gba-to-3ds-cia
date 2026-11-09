@ SPDX-License-Identifier: MIT
@ Start-up code of the gba2cia demo ROM. Unlike the usual crt0, it leaves
@ 0x0C0-0x1FF free: the build script fills 0x100-0x14F with the same kind of
@ data table the Generation III Pokemon games keep there (icons, palettes,
@ names), which the dual screen player reads.
	.section .crt0, "ax"
	.arm
	.global _start
_start:
	b	reset
	.fill	0xBC, 1, 0		@ 0x004-0x0BF: cartridge header (gbafix)
	.fill	0x140, 1, 0		@ 0x0C0-0x1FF: data header (make_demo.py)
reset:
	mov	r0, #0x12		@ IRQ mode stack
	msr	cpsr_c, r0
	ldr	sp, =0x03007FA0
	mov	r0, #0x1F		@ system mode stack, below 0x03005000
	msr	cpsr_c, r0
	ldr	sp, =0x03004F00
	ldr	r0, =__data_lma		@ copy .data to IWRAM
	ldr	r1, =__data_start
	ldr	r2, =__data_end
1:	cmp	r1, r2
	ldrlo	r3, [r0], #4
	strlo	r3, [r1], #4
	blo	1b
	ldr	r1, =__bss_start	@ clear .bss
	ldr	r2, =__bss_end
	mov	r3, #0
2:	cmp	r1, r2
	strlo	r3, [r1], #4
	blo	2b
	ldr	r0, =main
	mov	lr, pc
	bx	r0
3:	b	3b
	.pool
