	.arch armv8-a
	.file	"ltng_17766.c"
	.text
	.section	.rodata
	.align	3
.LC2:
	.string	"tests/runtime/phase4_finally_control.ltng"
	.section	.data.rel.local,"aw"
	.align	3
	.type	_ltng_src_file, %object
	.size	_ltng_src_file, 8
_ltng_src_file:
	.xword	.LC2
	.local	_ltng_try_top
	.comm	_ltng_try_top,8,8
	.local	_ltng_fn_name
	.comm	_ltng_fn_name,2048,8
	.local	_ltng_fn_line
	.comm	_ltng_fn_line,1024,8
	.local	_ltng_fn_col
	.comm	_ltng_fn_col,1024,8
	.local	_ltng_stack_depth
	.comm	_ltng_stack_depth,4,4
	.local	_ltng_exc_type
	.comm	_ltng_exc_type,128,8
	.local	_ltng_exc_msg
	.comm	_ltng_exc_msg,1024,8
	.local	_ltng_exc_value
	.comm	_ltng_exc_value,2048,8
	.local	_ltng_exc_trace
	.comm	_ltng_exc_trace,8192,8
	.local	_ltng_exc_line
	.comm	_ltng_exc_line,4,4
	.local	_ltng_exc_col
	.comm	_ltng_exc_col,4,4
	.local	_ltng_exc_func
	.comm	_ltng_exc_func,256,8
	.section	.rodata
	.align	3
.LC3:
	.string	"?"
	.text
	.align	2
	.type	_ltng_fn_push, %function
_ltng_fn_push:
.LFB6:
	.cfi_startproc
	sub	sp, sp, #16
	.cfi_def_cfa_offset 16
	str	x0, [sp, 8]
	str	w1, [sp, 4]
	str	w2, [sp]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	cmp	w0, 255
	bgt	.L5
	ldr	x0, [sp, 8]
	cmp	x0, 0
	beq	.L3
	ldr	x0, [sp, 8]
	b	.L4
.L3:
	adrp	x0, .LC3
	add	x0, x0, :lo12:.LC3
.L4:
	adrp	x1, _ltng_stack_depth
	add	x1, x1, :lo12:_ltng_stack_depth
	ldr	w2, [x1]
	adrp	x1, _ltng_fn_name
	add	x1, x1, :lo12:_ltng_fn_name
	sxtw	x2, w2
	str	x0, [x1, x2, lsl 3]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w1, [x0]
	adrp	x0, _ltng_fn_line
	add	x0, x0, :lo12:_ltng_fn_line
	sxtw	x1, w1
	ldr	w2, [sp, 4]
	str	w2, [x0, x1, lsl 2]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w1, [x0]
	adrp	x0, _ltng_fn_col
	add	x0, x0, :lo12:_ltng_fn_col
	sxtw	x1, w1
	ldr	w2, [sp]
	str	w2, [x0, x1, lsl 2]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	add	w1, w0, 1
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	str	w1, [x0]
.L5:
	nop
	add	sp, sp, 16
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE6:
	.size	_ltng_fn_push, .-_ltng_fn_push
	.align	2
	.type	_ltng_fn_pop, %function
_ltng_fn_pop:
.LFB7:
	.cfi_startproc
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	cmp	w0, 0
	ble	.L8
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	sub	w1, w0, #1
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	str	w1, [x0]
.L8:
	nop
	ret
	.cfi_endproc
.LFE7:
	.size	_ltng_fn_pop, .-_ltng_fn_pop
	.section	.rodata
	.align	3
.LC4:
	.string	"%s"
	.align	3
.LC5:
	.string	"  at %s (%s:%d:%d)\n"
	.align	3
.LC6:
	.string	"  at <top-level> (%s:%d:%d)\n"
	.text
	.align	2
	.type	_ltng_capture_trace, %function
_ltng_capture_trace:
.LFB8:
	.cfi_startproc
	stp	x29, x30, [sp, -16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	sub	sp, sp, #544
	.cfi_def_cfa_offset 560
	str	w0, [sp, 12]
	str	w1, [sp, 8]
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x1, [x0]
	str	x1, [sp, 536]
	mov	x1, 0
	adrp	x0, _ltng_exc_line
	add	x0, x0, :lo12:_ltng_exc_line
	ldr	w1, [sp, 12]
	str	w1, [x0]
	adrp	x0, _ltng_exc_col
	add	x0, x0, :lo12:_ltng_exc_col
	ldr	w1, [sp, 8]
	str	w1, [x0]
	adrp	x0, _ltng_exc_func
	add	x0, x0, :lo12:_ltng_exc_func
	strb	wzr, [x0]
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	strb	wzr, [x0]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	cmp	w0, 0
	ble	.L10
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	sub	w1, w0, #1
	adrp	x0, _ltng_fn_name
	add	x0, x0, :lo12:_ltng_fn_name
	sxtw	x1, w1
	ldr	x0, [x0, x1, lsl 3]
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	adrp	x0, _ltng_exc_func
	add	x0, x0, :lo12:_ltng_exc_func
	bl	snprintf
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	sub	w1, w0, #1
	adrp	x0, _ltng_fn_name
	add	x0, x0, :lo12:_ltng_fn_name
	sxtw	x1, w1
	ldr	x1, [x0, x1, lsl 3]
	adrp	x0, _ltng_src_file
	add	x0, x0, :lo12:_ltng_src_file
	ldr	x0, [x0]
	add	x7, sp, 24
	ldr	w6, [sp, 8]
	ldr	w5, [sp, 12]
	mov	x4, x0
	mov	x3, x1
	adrp	x0, .LC5
	add	x2, x0, :lo12:.LC5
	mov	x1, 512
	mov	x0, x7
	bl	snprintf
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	strlen
	mov	x1, x0
	mov	x0, 8191
	sub	x1, x0, x1
	add	x0, sp, 24
	mov	x2, x1
	mov	x1, x0
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	strncat
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	sub	w0, w0, #2
	str	w0, [sp, 20]
	b	.L11
.L12:
	adrp	x0, _ltng_fn_name
	add	x0, x0, :lo12:_ltng_fn_name
	ldrsw	x1, [sp, 20]
	ldr	x2, [x0, x1, lsl 3]
	adrp	x0, _ltng_src_file
	add	x0, x0, :lo12:_ltng_src_file
	ldr	x3, [x0]
	ldr	w0, [sp, 20]
	add	w1, w0, 1
	adrp	x0, _ltng_fn_line
	add	x0, x0, :lo12:_ltng_fn_line
	sxtw	x1, w1
	ldr	w4, [x0, x1, lsl 2]
	ldr	w0, [sp, 20]
	add	w1, w0, 1
	adrp	x0, _ltng_fn_col
	add	x0, x0, :lo12:_ltng_fn_col
	sxtw	x1, w1
	ldr	w0, [x0, x1, lsl 2]
	add	x7, sp, 24
	mov	w6, w0
	mov	w5, w4
	mov	x4, x3
	mov	x3, x2
	adrp	x0, .LC5
	add	x2, x0, :lo12:.LC5
	mov	x1, 512
	mov	x0, x7
	bl	snprintf
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	strlen
	mov	x1, x0
	mov	x0, 8191
	sub	x1, x0, x1
	add	x0, sp, 24
	mov	x2, x1
	mov	x1, x0
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	strncat
	ldr	w0, [sp, 20]
	sub	w0, w0, #1
	str	w0, [sp, 20]
.L11:
	ldr	w0, [sp, 20]
	cmp	w0, 0
	bge	.L12
	b	.L15
.L10:
	adrp	x0, _ltng_src_file
	add	x0, x0, :lo12:_ltng_src_file
	ldr	x0, [x0]
	add	x6, sp, 24
	ldr	w5, [sp, 8]
	ldr	w4, [sp, 12]
	mov	x3, x0
	adrp	x0, .LC6
	add	x2, x0, :lo12:.LC6
	mov	x1, 512
	mov	x0, x6
	bl	snprintf
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	strlen
	mov	x1, x0
	mov	x0, 8191
	sub	x1, x0, x1
	add	x0, sp, 24
	mov	x2, x1
	mov	x1, x0
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	strncat
.L15:
	nop
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x2, [sp, 536]
	ldr	x1, [x0]
	subs	x2, x2, x1
	mov	x1, 0
	beq	.L14
	bl	__stack_chk_fail
.L14:
	add	sp, sp, 544
	.cfi_def_cfa_offset 16
	ldp	x29, x30, [sp], 16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE8:
	.size	_ltng_capture_trace, .-_ltng_capture_trace
	.align	2
	.type	_ltng_exc_save_to, %function
_ltng_exc_save_to:
.LFB9:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	ldr	x4, [sp, 24]
	adrp	x0, _ltng_exc_type
	add	x3, x0, :lo12:_ltng_exc_type
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 128
	mov	x0, x4
	bl	snprintf
	ldr	x0, [sp, 24]
	add	x4, x0, 128
	adrp	x0, _ltng_exc_msg
	add	x3, x0, :lo12:_ltng_exc_msg
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 1024
	mov	x0, x4
	bl	snprintf
	ldr	x0, [sp, 24]
	add	x4, x0, 1152
	adrp	x0, _ltng_exc_value
	add	x3, x0, :lo12:_ltng_exc_value
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 2048
	mov	x0, x4
	bl	snprintf
	ldr	x0, [sp, 24]
	add	x4, x0, 3200
	adrp	x0, _ltng_exc_trace
	add	x3, x0, :lo12:_ltng_exc_trace
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 8192
	mov	x0, x4
	bl	snprintf
	adrp	x0, _ltng_exc_line
	add	x0, x0, :lo12:_ltng_exc_line
	ldr	w1, [x0]
	ldr	x0, [sp, 24]
	str	w1, [x0, 11392]
	adrp	x0, _ltng_exc_col
	add	x0, x0, :lo12:_ltng_exc_col
	ldr	w1, [x0]
	ldr	x0, [sp, 24]
	str	w1, [x0, 11396]
	ldr	x1, [sp, 24]
	mov	x0, 11400
	add	x4, x1, x0
	adrp	x0, _ltng_exc_func
	add	x3, x0, :lo12:_ltng_exc_func
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	mov	x0, x4
	bl	snprintf
	nop
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE9:
	.size	_ltng_exc_save_to, .-_ltng_exc_save_to
	.align	2
	.type	_ltng_exc_restore_from, %function
_ltng_exc_restore_from:
.LFB10:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	ldr	x0, [sp, 24]
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 128
	adrp	x0, _ltng_exc_type
	add	x0, x0, :lo12:_ltng_exc_type
	bl	snprintf
	ldr	x0, [sp, 24]
	add	x0, x0, 128
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 1024
	adrp	x0, _ltng_exc_msg
	add	x0, x0, :lo12:_ltng_exc_msg
	bl	snprintf
	ldr	x0, [sp, 24]
	add	x0, x0, 1152
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 2048
	adrp	x0, _ltng_exc_value
	add	x0, x0, :lo12:_ltng_exc_value
	bl	snprintf
	ldr	x0, [sp, 24]
	add	x0, x0, 3200
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 8192
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	snprintf
	ldr	x0, [sp, 24]
	ldr	w1, [x0, 11392]
	adrp	x0, _ltng_exc_line
	add	x0, x0, :lo12:_ltng_exc_line
	str	w1, [x0]
	ldr	x0, [sp, 24]
	ldr	w1, [x0, 11396]
	adrp	x0, _ltng_exc_col
	add	x0, x0, :lo12:_ltng_exc_col
	str	w1, [x0]
	ldr	x1, [sp, 24]
	mov	x0, 11400
	add	x0, x1, x0
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	adrp	x0, _ltng_exc_func
	add	x0, x0, :lo12:_ltng_exc_func
	bl	snprintf
	nop
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE10:
	.size	_ltng_exc_restore_from, .-_ltng_exc_restore_from
	.section	.rodata
	.align	3
.LC7:
	.string	"Error"
	.align	3
.LC8:
	.string	"%s: %s\n"
	.align	3
.LC9:
	.string	"%s\n"
	.text
	.align	2
	.type	_ltng_uncaught, %function
_ltng_uncaught:
.LFB11:
	.cfi_startproc
	stp	x29, x30, [sp, -16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	adrp	x0, _ltng_exc_msg
	add	x0, x0, :lo12:_ltng_exc_msg
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L19
	adrp	x0, :got:stderr
	ldr	x0, [x0, :got_lo12:stderr]
	ldr	x4, [x0]
	adrp	x0, _ltng_exc_type
	add	x0, x0, :lo12:_ltng_exc_type
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L20
	adrp	x0, _ltng_exc_type
	add	x0, x0, :lo12:_ltng_exc_type
	b	.L21
.L20:
	adrp	x0, .LC7
	add	x0, x0, :lo12:.LC7
.L21:
	adrp	x1, _ltng_exc_msg
	add	x3, x1, :lo12:_ltng_exc_msg
	mov	x2, x0
	adrp	x0, .LC8
	add	x1, x0, :lo12:.LC8
	mov	x0, x4
	bl	fprintf
	b	.L22
.L19:
	adrp	x0, :got:stderr
	ldr	x0, [x0, :got_lo12:stderr]
	ldr	x3, [x0]
	adrp	x0, _ltng_exc_type
	add	x0, x0, :lo12:_ltng_exc_type
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L23
	adrp	x0, _ltng_exc_type
	add	x0, x0, :lo12:_ltng_exc_type
	b	.L24
.L23:
	adrp	x0, .LC7
	add	x0, x0, :lo12:.LC7
.L24:
	mov	x2, x0
	adrp	x0, .LC9
	add	x1, x0, :lo12:.LC9
	mov	x0, x3
	bl	fprintf
.L22:
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L25
	adrp	x0, :got:stderr
	ldr	x0, [x0, :got_lo12:stderr]
	ldr	x0, [x0]
	mov	x1, x0
	adrp	x0, _ltng_exc_trace
	add	x0, x0, :lo12:_ltng_exc_trace
	bl	fputs
.L25:
	adrp	x0, :got:stderr
	ldr	x0, [x0, :got_lo12:stderr]
	ldr	x0, [x0]
	bl	fflush
	mov	w0, 1
	bl	exit
	.cfi_endproc
.LFE11:
	.size	_ltng_uncaught, .-_ltng_uncaught
	.section	.rodata
	.align	3
.LC0:
	.string	""
	.text
	.align	2
	.type	_ltng_throw_value, %function
_ltng_throw_value:
.LFB12:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 40]
	str	x1, [sp, 32]
	str	w2, [sp, 28]
	str	w3, [sp, 24]
	ldr	x0, [sp, 40]
	cmp	x0, 0
	beq	.L27
	ldr	x0, [sp, 40]
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L27
	ldr	x0, [sp, 40]
	b	.L28
.L27:
	adrp	x0, .LC7
	add	x0, x0, :lo12:.LC7
.L28:
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 128
	adrp	x0, _ltng_exc_type
	add	x0, x0, :lo12:_ltng_exc_type
	bl	snprintf
	ldr	x0, [sp, 32]
	cmp	x0, 0
	beq	.L29
	ldr	x0, [sp, 32]
	b	.L30
.L29:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L30:
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 1024
	adrp	x0, _ltng_exc_msg
	add	x0, x0, :lo12:_ltng_exc_msg
	bl	snprintf
	ldr	x0, [sp, 32]
	cmp	x0, 0
	beq	.L31
	ldr	x0, [sp, 32]
	b	.L32
.L31:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L32:
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 2048
	adrp	x0, _ltng_exc_value
	add	x0, x0, :lo12:_ltng_exc_value
	bl	snprintf
	ldr	w1, [sp, 24]
	ldr	w0, [sp, 28]
	bl	_ltng_capture_trace
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	ldr	x0, [x0]
	cmp	x0, 0
	bne	.L33
	bl	_ltng_uncaught
.L33:
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	ldr	x0, [x0]
	mov	w1, 1
	bl	longjmp
	.cfi_endproc
.LFE12:
	.size	_ltng_throw_value, .-_ltng_throw_value
	.align	2
	.type	_ltng_rethrow, %function
_ltng_rethrow:
.LFB13:
	.cfi_startproc
	stp	x29, x30, [sp, -16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	ldr	x0, [x0]
	cmp	x0, 0
	bne	.L35
	bl	_ltng_uncaught
.L35:
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	ldr	x0, [x0]
	mov	w1, 1
	bl	longjmp
	.cfi_endproc
.LFE13:
	.size	_ltng_rethrow, .-_ltng_rethrow
	.align	2
	.type	_ltng_concat, %function
_ltng_concat:
.LFB14:
	.cfi_startproc
	sub	sp, sp, #288
	.cfi_def_cfa_offset 288
	stp	x29, x30, [sp, 96]
	.cfi_offset 29, -192
	.cfi_offset 30, -184
	add	x29, sp, 96
	str	x0, [sp, 24]
	str	x1, [sp, 16]
	str	w2, [sp, 12]
	str	x3, [sp, 248]
	str	x4, [sp, 256]
	str	x5, [sp, 264]
	str	x6, [sp, 272]
	str	x7, [sp, 280]
	str	q0, [sp, 112]
	str	q1, [sp, 128]
	str	q2, [sp, 144]
	str	q3, [sp, 160]
	str	q4, [sp, 176]
	str	q5, [sp, 192]
	str	q6, [sp, 208]
	str	q7, [sp, 224]
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x1, [x0]
	str	x1, [sp, 88]
	mov	x1, 0
	add	x0, sp, 288
	str	x0, [sp, 56]
	add	x0, sp, 288
	str	x0, [sp, 64]
	add	x0, sp, 240
	str	x0, [sp, 72]
	mov	w0, -40
	str	w0, [sp, 80]
	mov	w0, -128
	str	w0, [sp, 84]
	ldr	x0, [sp, 24]
	strb	wzr, [x0]
	str	wzr, [sp, 44]
	b	.L37
.L41:
	ldr	w1, [sp, 80]
	ldr	x0, [sp, 56]
	cmp	w1, 0
	blt	.L38
	add	x1, x0, 15
	and	x1, x1, -8
	str	x1, [sp, 56]
	b	.L39
.L38:
	add	w2, w1, 8
	str	w2, [sp, 80]
	ldr	w2, [sp, 80]
	cmp	w2, 0
	ble	.L40
	add	x1, x0, 15
	and	x1, x1, -8
	str	x1, [sp, 56]
	b	.L39
.L40:
	ldr	x2, [sp, 64]
	sxtw	x0, w1
	add	x0, x2, x0
.L39:
	ldr	x0, [x0]
	str	x0, [sp, 48]
	ldr	x0, [sp, 24]
	bl	strlen
	mov	x1, x0
	ldr	x0, [sp, 16]
	sub	x0, x0, x1
	sub	x0, x0, #1
	mov	x2, x0
	ldr	x1, [sp, 48]
	ldr	x0, [sp, 24]
	bl	strncat
	ldr	w0, [sp, 44]
	add	w0, w0, 1
	str	w0, [sp, 44]
.L37:
	ldr	w1, [sp, 44]
	ldr	w0, [sp, 12]
	cmp	w1, w0
	blt	.L41
	ldr	x0, [sp, 24]
	mov	x1, x0
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x3, [sp, 88]
	ldr	x2, [x0]
	subs	x3, x3, x2
	mov	x2, 0
	beq	.L43
	bl	__stack_chk_fail
.L43:
	mov	x0, x1
	ldp	x29, x30, [sp, 96]
	add	sp, sp, 288
	.cfi_restore 29
	.cfi_restore 30
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE14:
	.size	_ltng_concat, .-_ltng_concat
	.align	2
	.type	_ltng_repeat, %function
_ltng_repeat:
.LFB15:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 40]
	str	x1, [sp, 32]
	str	x2, [sp, 24]
	str	x3, [sp, 16]
	ldr	x0, [sp, 40]
	strb	wzr, [x0]
	ldr	x0, [sp, 16]
	cmp	x0, 0
	bge	.L45
	str	xzr, [sp, 16]
.L45:
	str	xzr, [sp, 56]
	b	.L46
.L48:
	ldr	x0, [sp, 40]
	bl	strlen
	mov	x1, x0
	ldr	x0, [sp, 32]
	sub	x0, x0, x1
	sub	x0, x0, #1
	mov	x2, x0
	ldr	x1, [sp, 24]
	ldr	x0, [sp, 40]
	bl	strncat
	ldr	x0, [sp, 56]
	add	x0, x0, 1
	str	x0, [sp, 56]
.L46:
	ldr	x1, [sp, 56]
	ldr	x0, [sp, 16]
	cmp	x1, x0
	bge	.L47
	ldr	x0, [sp, 40]
	bl	strlen
	mov	x1, x0
	ldr	x0, [sp, 32]
	sub	x0, x0, #1
	cmp	x1, x0
	bcc	.L48
.L47:
	ldr	x0, [sp, 40]
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE15:
	.size	_ltng_repeat, .-_ltng_repeat
	.align	2
	.type	_ltng_arr_strdup, %function
_ltng_arr_strdup:
.LFB16:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 24]
	ldr	x0, [sp, 24]
	cmp	x0, 0
	bne	.L51
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 24]
.L51:
	ldr	x0, [sp, 24]
	bl	strlen
	add	x0, x0, 1
	str	x0, [sp, 32]
	ldr	x0, [sp, 32]
	bl	malloc
	str	x0, [sp, 40]
	ldr	x0, [sp, 40]
	cmp	x0, 0
	beq	.L52
	ldr	x2, [sp, 32]
	ldr	x1, [sp, 24]
	ldr	x0, [sp, 40]
	bl	memcpy
.L52:
	ldr	x0, [sp, 40]
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE16:
	.size	_ltng_arr_strdup, .-_ltng_arr_strdup
	.align	2
	.type	_ltng_arr_new, %function
_ltng_arr_new:
.LFB17:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	w0, [sp, 28]
	ldr	w0, [sp, 28]
	cmp	w0, 0
	bgt	.L55
	mov	w0, 1
	str	w0, [sp, 28]
.L55:
	ldrsw	x0, [sp, 28]
	lsl	x0, x0, 3
	bl	malloc
	str	x0, [sp, 40]
	str	wzr, [sp, 36]
	b	.L56
.L57:
	ldrsw	x0, [sp, 36]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	str	xzr, [x0]
	ldr	w0, [sp, 36]
	add	w0, w0, 1
	str	w0, [sp, 36]
.L56:
	ldr	w1, [sp, 36]
	ldr	w0, [sp, 28]
	cmp	w1, w0
	blt	.L57
	ldr	x0, [sp, 40]
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE17:
	.size	_ltng_arr_new, .-_ltng_arr_new
	.align	2
	.type	_ltng_arr_ensure, %function
_ltng_arr_ensure:
.LFB18:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 40]
	str	x1, [sp, 32]
	str	x2, [sp, 24]
	str	w3, [sp, 20]
	ldr	x0, [sp, 24]
	ldr	w0, [x0]
	ldr	w1, [sp, 20]
	cmp	w1, w0
	ble	.L68
	ldr	x0, [sp, 24]
	ldr	w0, [x0]
	cmp	w0, 0
	beq	.L62
	ldr	x0, [sp, 24]
	ldr	w0, [x0]
	lsl	w0, w0, 1
	str	w0, [sp, 56]
	b	.L64
.L62:
	mov	w0, 1
	str	w0, [sp, 56]
	b	.L64
.L65:
	ldr	w0, [sp, 56]
	lsl	w0, w0, 1
	str	w0, [sp, 56]
.L64:
	ldr	w1, [sp, 56]
	ldr	w0, [sp, 20]
	cmp	w1, w0
	blt	.L65
	ldr	x0, [sp, 40]
	ldr	x2, [x0]
	ldrsw	x0, [sp, 56]
	lsl	x0, x0, 3
	mov	x1, x0
	mov	x0, x2
	bl	realloc
	mov	x1, x0
	ldr	x0, [sp, 40]
	str	x1, [x0]
	ldr	x0, [sp, 24]
	ldr	w0, [x0]
	str	w0, [sp, 60]
	b	.L66
.L67:
	ldr	x0, [sp, 40]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 60]
	lsl	x0, x0, 3
	add	x0, x1, x0
	str	xzr, [x0]
	ldr	w0, [sp, 60]
	add	w0, w0, 1
	str	w0, [sp, 60]
.L66:
	ldr	w1, [sp, 60]
	ldr	w0, [sp, 56]
	cmp	w1, w0
	blt	.L67
	ldr	x0, [sp, 24]
	ldr	w1, [sp, 56]
	str	w1, [x0]
	b	.L59
.L68:
	nop
.L59:
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE18:
	.size	_ltng_arr_ensure, .-_ltng_arr_ensure
	.align	2
	.type	_ltng_arr_set_elem, %function
_ltng_arr_set_elem:
.LFB19:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 40]
	str	w1, [sp, 36]
	str	x2, [sp, 24]
	ldrsw	x0, [sp, 36]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L70
	ldrsw	x0, [sp, 36]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
.L70:
	ldr	x0, [sp, 24]
	cmp	x0, 0
	beq	.L71
	ldr	x0, [sp, 24]
	bl	_ltng_arr_strdup
	mov	x2, x0
	b	.L72
.L71:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	bl	_ltng_arr_strdup
	mov	x2, x0
.L72:
	ldrsw	x0, [sp, 36]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	str	x2, [x0]
	nop
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE19:
	.size	_ltng_arr_set_elem, .-_ltng_arr_set_elem
	.align	2
	.type	_ltng_arr_get, %function
_ltng_arr_get:
.LFB20:
	.cfi_startproc
	sub	sp, sp, #16
	.cfi_def_cfa_offset 16
	str	x0, [sp, 8]
	str	w1, [sp, 4]
	str	w2, [sp]
	ldr	w0, [sp]
	cmp	w0, 0
	blt	.L74
	ldr	w1, [sp]
	ldr	w0, [sp, 4]
	cmp	w1, w0
	bge	.L74
	ldrsw	x0, [sp]
	lsl	x0, x0, 3
	ldr	x1, [sp, 8]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L75
	ldrsw	x0, [sp]
	lsl	x0, x0, 3
	ldr	x1, [sp, 8]
	add	x0, x1, x0
	ldr	x0, [x0]
	b	.L77
.L75:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	b	.L77
.L74:
	ldr	w0, [sp]
	cmp	w0, 0
	bge	.L78
	ldr	w1, [sp, 4]
	ldr	w0, [sp]
	add	w0, w1, w0
	cmp	w0, 0
	blt	.L78
	ldr	w1, [sp, 4]
	ldr	w0, [sp]
	add	w0, w1, w0
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 8]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L79
	ldr	w1, [sp, 4]
	ldr	w0, [sp]
	add	w0, w1, w0
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 8]
	add	x0, x1, x0
	ldr	x0, [x0]
	b	.L77
.L79:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	b	.L77
.L78:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L77:
	add	sp, sp, 16
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE20:
	.size	_ltng_arr_get, .-_ltng_arr_get
	.align	2
	.type	_ltng_arr_set, %function
_ltng_arr_set:
.LFB21:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 56]
	str	x1, [sp, 48]
	str	x2, [sp, 40]
	str	w3, [sp, 36]
	str	x4, [sp, 24]
	ldr	w0, [sp, 36]
	cmp	w0, 0
	blt	.L85
	ldr	x0, [sp, 48]
	ldr	w0, [x0]
	ldr	w1, [sp, 36]
	cmp	w1, w0
	bge	.L85
	ldr	x2, [sp, 24]
	ldr	w1, [sp, 36]
	ldr	x0, [sp, 56]
	bl	_ltng_arr_set_elem
	b	.L81
.L85:
	nop
.L81:
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE21:
	.size	_ltng_arr_set, .-_ltng_arr_set
	.align	2
	.type	_ltng_arr_push, %function
_ltng_arr_push:
.LFB22:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 40]
	str	x1, [sp, 32]
	str	x2, [sp, 24]
	str	x3, [sp, 16]
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	add	w0, w0, 1
	mov	w3, w0
	ldr	x2, [sp, 24]
	ldr	x1, [sp, 32]
	ldr	x0, [sp, 40]
	bl	_ltng_arr_ensure
	ldr	x0, [sp, 40]
	ldr	x3, [x0]
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	ldr	x2, [sp, 16]
	mov	w1, w0
	mov	x0, x3
	bl	_ltng_arr_set_elem
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	add	w1, w0, 1
	ldr	x0, [sp, 32]
	str	w1, [x0]
	nop
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE22:
	.size	_ltng_arr_push, .-_ltng_arr_push
	.align	2
	.type	_ltng_arr_pop, %function
_ltng_arr_pop:
.LFB23:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 24]
	str	x1, [sp, 16]
	ldr	x0, [sp, 16]
	ldr	w0, [x0]
	cmp	w0, 0
	bgt	.L88
	adrp	x0, b.7
	add	x0, x0, :lo12:b.7
	strb	wzr, [x0]
	adrp	x0, b.7
	add	x0, x0, :lo12:b.7
	b	.L89
.L88:
	ldr	x0, [sp, 16]
	ldr	w0, [x0]
	sub	w1, w0, #1
	ldr	x0, [sp, 16]
	str	w1, [x0]
	ldr	x0, [sp, 16]
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 24]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L90
	ldr	x0, [sp, 16]
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 24]
	add	x0, x1, x0
	ldr	x0, [x0]
	str	x0, [sp, 40]
	b	.L91
.L90:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 40]
.L91:
	ldr	x3, [sp, 40]
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	adrp	x0, b.7
	add	x0, x0, :lo12:b.7
	bl	snprintf
	ldr	x0, [sp, 16]
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 24]
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
	ldr	x0, [sp, 16]
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 24]
	add	x0, x1, x0
	str	xzr, [x0]
	adrp	x0, b.7
	add	x0, x0, :lo12:b.7
.L89:
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE23:
	.size	_ltng_arr_pop, .-_ltng_arr_pop
	.align	2
	.type	_ltng_arr_pop_index, %function
_ltng_arr_pop_index:
.LFB24:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 40]
	str	x1, [sp, 32]
	str	w2, [sp, 28]
	ldr	w0, [sp, 28]
	cmp	w0, 0
	ble	.L93
	ldr	w0, [sp, 28]
	sub	w0, w0, #1
	str	w0, [sp, 28]
	b	.L94
.L93:
	ldr	w0, [sp, 28]
	cmp	w0, 0
	bge	.L94
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	ldr	w1, [sp, 28]
	add	w0, w1, w0
	str	w0, [sp, 28]
.L94:
	ldr	w0, [sp, 28]
	cmp	w0, 0
	blt	.L95
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	ldr	w1, [sp, 28]
	cmp	w1, w0
	blt	.L96
.L95:
	adrp	x0, b.6
	add	x0, x0, :lo12:b.6
	strb	wzr, [x0]
	adrp	x0, b.6
	add	x0, x0, :lo12:b.6
	b	.L97
.L96:
	ldrsw	x0, [sp, 28]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L98
	ldrsw	x0, [sp, 28]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	str	x0, [sp, 56]
	b	.L99
.L98:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 56]
.L99:
	ldr	x3, [sp, 56]
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	adrp	x0, b.6
	add	x0, x0, :lo12:b.6
	bl	snprintf
	ldrsw	x0, [sp, 28]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
	ldr	w0, [sp, 28]
	str	w0, [sp, 52]
	b	.L100
.L101:
	ldrsw	x0, [sp, 52]
	add	x0, x0, 1
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x1, x1, x0
	ldrsw	x0, [sp, 52]
	lsl	x0, x0, 3
	ldr	x2, [sp, 40]
	add	x0, x2, x0
	ldr	x1, [x1]
	str	x1, [x0]
	ldr	w0, [sp, 52]
	add	w0, w0, 1
	str	w0, [sp, 52]
.L100:
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	sub	w0, w0, #1
	ldr	w1, [sp, 52]
	cmp	w1, w0
	blt	.L101
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x0, x0, 3
	sub	x0, x0, #8
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	str	xzr, [x0]
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	sub	w1, w0, #1
	ldr	x0, [sp, 32]
	str	w1, [x0]
	adrp	x0, b.6
	add	x0, x0, :lo12:b.6
.L97:
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE24:
	.size	_ltng_arr_pop_index, .-_ltng_arr_pop_index
	.align	2
	.type	_ltng_arr_insert, %function
_ltng_arr_insert:
.LFB25:
	.cfi_startproc
	stp	x29, x30, [sp, -80]!
	.cfi_def_cfa_offset 80
	.cfi_offset 29, -80
	.cfi_offset 30, -72
	mov	x29, sp
	str	x0, [sp, 56]
	str	x1, [sp, 48]
	str	x2, [sp, 40]
	str	w3, [sp, 36]
	str	x4, [sp, 24]
	ldr	w0, [sp, 36]
	cmp	w0, 0
	ble	.L103
	ldr	w0, [sp, 36]
	sub	w0, w0, #1
	str	w0, [sp, 36]
	b	.L104
.L103:
	ldr	w0, [sp, 36]
	cmp	w0, 0
	bge	.L104
	ldr	x0, [sp, 48]
	ldr	w0, [x0]
	ldr	w1, [sp, 36]
	add	w0, w1, w0
	str	w0, [sp, 36]
.L104:
	ldr	w0, [sp, 36]
	cmp	w0, 0
	bge	.L105
	str	wzr, [sp, 36]
.L105:
	ldr	x0, [sp, 48]
	ldr	w0, [x0]
	ldr	w1, [sp, 36]
	cmp	w1, w0
	ble	.L106
	ldr	x0, [sp, 48]
	ldr	w0, [x0]
	str	w0, [sp, 36]
.L106:
	ldr	x0, [sp, 48]
	ldr	w0, [x0]
	add	w0, w0, 1
	mov	w3, w0
	ldr	x2, [sp, 40]
	ldr	x1, [sp, 48]
	ldr	x0, [sp, 56]
	bl	_ltng_arr_ensure
	ldr	x0, [sp, 48]
	ldr	w0, [x0]
	str	w0, [sp, 76]
	b	.L107
.L108:
	ldr	x0, [sp, 56]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 76]
	lsl	x0, x0, 3
	sub	x0, x0, #8
	add	x1, x1, x0
	ldr	x0, [sp, 56]
	ldr	x2, [x0]
	ldrsw	x0, [sp, 76]
	lsl	x0, x0, 3
	add	x0, x2, x0
	ldr	x1, [x1]
	str	x1, [x0]
	ldr	w0, [sp, 76]
	sub	w0, w0, #1
	str	w0, [sp, 76]
.L107:
	ldr	w1, [sp, 76]
	ldr	w0, [sp, 36]
	cmp	w1, w0
	bgt	.L108
	ldr	x0, [sp, 56]
	ldr	x0, [x0]
	ldr	x2, [sp, 24]
	ldr	w1, [sp, 36]
	bl	_ltng_arr_set_elem
	ldr	x0, [sp, 48]
	ldr	w0, [x0]
	add	w1, w0, 1
	ldr	x0, [sp, 48]
	str	w1, [x0]
	nop
	ldp	x29, x30, [sp], 80
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE25:
	.size	_ltng_arr_insert, .-_ltng_arr_insert
	.align	2
	.type	_ltng_arr_remove, %function
_ltng_arr_remove:
.LFB26:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 40]
	str	x1, [sp, 32]
	str	w2, [sp, 28]
	ldr	w0, [sp, 28]
	cmp	w0, 0
	ble	.L110
	ldr	w0, [sp, 28]
	sub	w0, w0, #1
	str	w0, [sp, 28]
	b	.L111
.L110:
	ldr	w0, [sp, 28]
	cmp	w0, 0
	bge	.L111
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	ldr	w1, [sp, 28]
	add	w0, w1, w0
	str	w0, [sp, 28]
.L111:
	ldr	w0, [sp, 28]
	cmp	w0, 0
	blt	.L117
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	ldr	w1, [sp, 28]
	cmp	w1, w0
	bge	.L117
	ldrsw	x0, [sp, 28]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
	ldr	w0, [sp, 28]
	str	w0, [sp, 60]
	b	.L115
.L116:
	ldrsw	x0, [sp, 60]
	add	x0, x0, 1
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x1, x1, x0
	ldrsw	x0, [sp, 60]
	lsl	x0, x0, 3
	ldr	x2, [sp, 40]
	add	x0, x2, x0
	ldr	x1, [x1]
	str	x1, [x0]
	ldr	w0, [sp, 60]
	add	w0, w0, 1
	str	w0, [sp, 60]
.L115:
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	sub	w0, w0, #1
	ldr	w1, [sp, 60]
	cmp	w1, w0
	blt	.L116
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x0, x0, 3
	sub	x0, x0, #8
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	str	xzr, [x0]
	ldr	x0, [sp, 32]
	ldr	w0, [x0]
	sub	w1, w0, #1
	ldr	x0, [sp, 32]
	str	w1, [x0]
	b	.L109
.L117:
	nop
.L109:
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE26:
	.size	_ltng_arr_remove, .-_ltng_arr_remove
	.align	2
	.type	_ltng_arr_contains, %function
_ltng_arr_contains:
.LFB27:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 40]
	str	w1, [sp, 36]
	str	x2, [sp, 24]
	str	wzr, [sp, 60]
	b	.L119
.L122:
	ldrsw	x0, [sp, 60]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L120
	ldrsw	x0, [sp, 60]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	ldr	x1, [sp, 24]
	bl	strcmp
	cmp	w0, 0
	bne	.L120
	mov	w0, 1
	b	.L121
.L120:
	ldr	w0, [sp, 60]
	add	w0, w0, 1
	str	w0, [sp, 60]
.L119:
	ldr	w1, [sp, 60]
	ldr	w0, [sp, 36]
	cmp	w1, w0
	blt	.L122
	mov	w0, 0
.L121:
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE27:
	.size	_ltng_arr_contains, .-_ltng_arr_contains
	.align	2
	.type	_ltng_arr_index_of, %function
_ltng_arr_index_of:
.LFB28:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 40]
	str	w1, [sp, 36]
	str	x2, [sp, 24]
	str	wzr, [sp, 60]
	b	.L124
.L127:
	ldrsw	x0, [sp, 60]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L125
	ldrsw	x0, [sp, 60]
	lsl	x0, x0, 3
	ldr	x1, [sp, 40]
	add	x0, x1, x0
	ldr	x0, [x0]
	ldr	x1, [sp, 24]
	bl	strcmp
	cmp	w0, 0
	bne	.L125
	ldr	w0, [sp, 60]
	add	w0, w0, 1
	b	.L126
.L125:
	ldr	w0, [sp, 60]
	add	w0, w0, 1
	str	w0, [sp, 60]
.L124:
	ldr	w1, [sp, 60]
	ldr	w0, [sp, 36]
	cmp	w1, w0
	blt	.L127
	mov	w0, 0
.L126:
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE28:
	.size	_ltng_arr_index_of, .-_ltng_arr_index_of
	.align	2
	.type	_ltng_arr_reverse, %function
_ltng_arr_reverse:
.LFB29:
	.cfi_startproc
	sub	sp, sp, #32
	.cfi_def_cfa_offset 32
	str	x0, [sp, 8]
	str	w1, [sp, 4]
	str	wzr, [sp, 16]
	ldr	w0, [sp, 4]
	sub	w0, w0, #1
	str	w0, [sp, 20]
	b	.L129
.L130:
	ldrsw	x0, [sp, 16]
	lsl	x0, x0, 3
	ldr	x1, [sp, 8]
	add	x0, x1, x0
	ldr	x0, [x0]
	str	x0, [sp, 24]
	ldrsw	x0, [sp, 20]
	lsl	x0, x0, 3
	ldr	x1, [sp, 8]
	add	x1, x1, x0
	ldrsw	x0, [sp, 16]
	lsl	x0, x0, 3
	ldr	x2, [sp, 8]
	add	x0, x2, x0
	ldr	x1, [x1]
	str	x1, [x0]
	ldrsw	x0, [sp, 20]
	lsl	x0, x0, 3
	ldr	x1, [sp, 8]
	add	x0, x1, x0
	ldr	x1, [sp, 24]
	str	x1, [x0]
	ldr	w0, [sp, 16]
	add	w0, w0, 1
	str	w0, [sp, 16]
	ldr	w0, [sp, 20]
	sub	w0, w0, #1
	str	w0, [sp, 20]
.L129:
	ldr	w1, [sp, 16]
	ldr	w0, [sp, 20]
	cmp	w1, w0
	blt	.L130
	nop
	nop
	add	sp, sp, 32
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE29:
	.size	_ltng_arr_reverse, .-_ltng_arr_reverse
	.align	2
	.type	_ltng_arr_numcmp, %function
_ltng_arr_numcmp:
.LFB30:
	.cfi_startproc
	sub	sp, sp, #80
	.cfi_def_cfa_offset 80
	stp	x29, x30, [sp, 64]
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	add	x29, sp, 64
	str	x0, [sp, 8]
	str	x1, [sp]
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x1, [x0]
	str	x1, [sp, 56]
	mov	x1, 0
	str	xzr, [sp, 24]
	str	xzr, [sp, 32]
	ldr	x0, [sp, 8]
	cmp	x0, 0
	beq	.L132
	ldr	x0, [sp, 8]
	b	.L133
.L132:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L133:
	add	x1, sp, 24
	bl	strtod
	str	d0, [sp, 40]
	ldr	x0, [sp]
	cmp	x0, 0
	beq	.L134
	ldr	x0, [sp]
	b	.L135
.L134:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L135:
	add	x1, sp, 32
	bl	strtod
	str	d0, [sp, 48]
	ldr	x0, [sp, 24]
	cmp	x0, 0
	beq	.L136
	ldr	x0, [sp, 24]
	ldrb	w0, [x0]
	cmp	w0, 0
	bne	.L136
	ldr	x0, [sp, 8]
	cmp	x0, 0
	beq	.L136
	ldr	x0, [sp, 8]
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L136
	mov	w0, 1
	b	.L137
.L136:
	mov	w0, 0
.L137:
	str	w0, [sp, 16]
	ldr	x0, [sp, 32]
	cmp	x0, 0
	beq	.L138
	ldr	x0, [sp, 32]
	ldrb	w0, [x0]
	cmp	w0, 0
	bne	.L138
	ldr	x0, [sp]
	cmp	x0, 0
	beq	.L138
	ldr	x0, [sp]
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L138
	mov	w0, 1
	b	.L139
.L138:
	mov	w0, 0
.L139:
	str	w0, [sp, 20]
	ldr	w0, [sp, 16]
	cmp	w0, 0
	beq	.L140
	ldr	w0, [sp, 20]
	cmp	w0, 0
	beq	.L140
	ldr	d30, [sp, 40]
	ldr	d31, [sp, 48]
	fcmpe	d30, d31
	bmi	.L152
	b	.L154
.L152:
	mov	w0, -1
	b	.L150
.L154:
	ldr	d30, [sp, 40]
	ldr	d31, [sp, 48]
	fcmpe	d30, d31
	bgt	.L153
	b	.L155
.L153:
	mov	w0, 1
	b	.L150
.L155:
	mov	w0, 0
	b	.L150
.L140:
	ldr	x0, [sp, 8]
	cmp	x0, 0
	beq	.L146
	ldr	x0, [sp, 8]
	b	.L147
.L146:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L147:
	ldr	x1, [sp]
	cmp	x1, 0
	beq	.L148
	ldr	x1, [sp]
	b	.L149
.L148:
	adrp	x1, .LC0
	add	x1, x1, :lo12:.LC0
.L149:
	bl	strcmp
.L150:
	mov	w1, w0
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x3, [sp, 56]
	ldr	x2, [x0]
	subs	x3, x3, x2
	mov	x2, 0
	beq	.L151
	bl	__stack_chk_fail
.L151:
	mov	w0, w1
	ldp	x29, x30, [sp, 64]
	add	sp, sp, 80
	.cfi_restore 29
	.cfi_restore 30
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE30:
	.size	_ltng_arr_numcmp, .-_ltng_arr_numcmp
	.align	2
	.type	_ltng_arr_cmp, %function
_ltng_arr_cmp:
.LFB31:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	str	x1, [sp, 16]
	ldr	x0, [sp, 24]
	ldr	x2, [x0]
	ldr	x0, [sp, 16]
	ldr	x0, [x0]
	mov	x1, x0
	mov	x0, x2
	bl	_ltng_arr_numcmp
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE31:
	.size	_ltng_arr_cmp, .-_ltng_arr_cmp
	.align	2
	.type	_ltng_arr_sort, %function
_ltng_arr_sort:
.LFB32:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	str	w1, [sp, 20]
	ldrsw	x1, [sp, 20]
	adrp	x0, _ltng_arr_cmp
	add	x3, x0, :lo12:_ltng_arr_cmp
	mov	x2, 8
	ldr	x0, [sp, 24]
	bl	qsort
	nop
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE32:
	.size	_ltng_arr_sort, .-_ltng_arr_sort
	.align	2
	.type	_ltng_arr_copy, %function
_ltng_arr_copy:
.LFB33:
	.cfi_startproc
	stp	x29, x30, [sp, -80]!
	.cfi_def_cfa_offset 80
	.cfi_offset 29, -80
	.cfi_offset 30, -72
	mov	x29, sp
	str	x0, [sp, 56]
	str	x1, [sp, 48]
	str	x2, [sp, 40]
	str	x3, [sp, 32]
	str	w4, [sp, 28]
	ldr	w3, [sp, 28]
	ldr	x2, [sp, 32]
	ldr	x1, [sp, 40]
	ldr	x0, [sp, 48]
	bl	_ltng_arr_ensure
	ldr	w0, [sp, 28]
	str	w0, [sp, 72]
	b	.L160
.L162:
	ldr	x0, [sp, 48]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 72]
	lsl	x0, x0, 3
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L161
	ldr	x0, [sp, 48]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 72]
	lsl	x0, x0, 3
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
	ldr	x0, [sp, 48]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 72]
	lsl	x0, x0, 3
	add	x0, x1, x0
	str	xzr, [x0]
.L161:
	ldr	w0, [sp, 72]
	add	w0, w0, 1
	str	w0, [sp, 72]
.L160:
	ldr	x0, [sp, 40]
	ldr	w0, [x0]
	ldr	w1, [sp, 72]
	cmp	w1, w0
	blt	.L162
	str	wzr, [sp, 76]
	b	.L163
.L167:
	ldr	x0, [sp, 48]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 76]
	lsl	x0, x0, 3
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L164
	ldr	x0, [sp, 48]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 76]
	lsl	x0, x0, 3
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
.L164:
	ldrsw	x0, [sp, 76]
	lsl	x0, x0, 3
	ldr	x1, [sp, 56]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L165
	ldrsw	x0, [sp, 76]
	lsl	x0, x0, 3
	ldr	x1, [sp, 56]
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	_ltng_arr_strdup
	mov	x2, x0
	b	.L166
.L165:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	bl	_ltng_arr_strdup
	mov	x2, x0
.L166:
	ldr	x0, [sp, 48]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 76]
	lsl	x0, x0, 3
	add	x0, x1, x0
	str	x2, [x0]
	ldr	w0, [sp, 76]
	add	w0, w0, 1
	str	w0, [sp, 76]
.L163:
	ldr	w1, [sp, 76]
	ldr	w0, [sp, 28]
	cmp	w1, w0
	blt	.L167
	ldr	x0, [sp, 40]
	ldr	w1, [sp, 28]
	str	w1, [x0]
	nop
	ldp	x29, x30, [sp], 80
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE33:
	.size	_ltng_arr_copy, .-_ltng_arr_copy
	.align	2
	.type	_ltng_arr_slice, %function
_ltng_arr_slice:
.LFB34:
	.cfi_startproc
	stp	x29, x30, [sp, -80]!
	.cfi_def_cfa_offset 80
	.cfi_offset 29, -80
	.cfi_offset 30, -72
	mov	x29, sp
	str	x0, [sp, 56]
	str	w1, [sp, 52]
	str	w2, [sp, 48]
	str	w3, [sp, 44]
	str	x4, [sp, 32]
	str	x5, [sp, 24]
	str	x6, [sp, 16]
	ldr	w0, [sp, 48]
	cmp	w0, 0
	ble	.L169
	ldr	w0, [sp, 48]
	sub	w0, w0, #1
	str	w0, [sp, 48]
	b	.L170
.L169:
	ldr	w0, [sp, 48]
	cmp	w0, 0
	bge	.L170
	ldr	w1, [sp, 48]
	ldr	w0, [sp, 52]
	add	w0, w1, w0
	str	w0, [sp, 48]
.L170:
	ldr	w0, [sp, 44]
	cmp	w0, 0
	ble	.L171
	ldr	w0, [sp, 44]
	sub	w0, w0, #1
	str	w0, [sp, 44]
	b	.L172
.L171:
	ldr	w0, [sp, 44]
	cmp	w0, 0
	bge	.L172
	ldr	w1, [sp, 44]
	ldr	w0, [sp, 52]
	add	w0, w1, w0
	str	w0, [sp, 44]
.L172:
	ldr	w0, [sp, 48]
	cmp	w0, 0
	bge	.L173
	str	wzr, [sp, 48]
.L173:
	ldr	w0, [sp, 44]
	cmp	w0, 0
	bge	.L174
	str	wzr, [sp, 44]
.L174:
	ldr	w1, [sp, 44]
	ldr	w0, [sp, 52]
	cmp	w1, w0
	ble	.L175
	ldr	w0, [sp, 52]
	str	w0, [sp, 44]
.L175:
	ldr	w1, [sp, 48]
	ldr	w0, [sp, 52]
	cmp	w1, w0
	ble	.L176
	ldr	w0, [sp, 52]
	str	w0, [sp, 48]
.L176:
	ldr	w1, [sp, 48]
	ldr	w0, [sp, 44]
	cmp	w1, w0
	ble	.L177
	ldr	w0, [sp, 44]
	str	w0, [sp, 48]
.L177:
	ldr	w1, [sp, 44]
	ldr	w0, [sp, 48]
	sub	w0, w1, w0
	str	w0, [sp, 76]
	ldr	w3, [sp, 76]
	ldr	x2, [sp, 16]
	ldr	x1, [sp, 24]
	ldr	x0, [sp, 32]
	bl	_ltng_arr_ensure
	str	wzr, [sp, 68]
	b	.L178
.L180:
	ldr	x0, [sp, 32]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 68]
	lsl	x0, x0, 3
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L179
	ldr	x0, [sp, 32]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 68]
	lsl	x0, x0, 3
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
	ldr	x0, [sp, 32]
	ldr	x1, [x0]
	ldrsw	x0, [sp, 68]
	lsl	x0, x0, 3
	add	x0, x1, x0
	str	xzr, [x0]
.L179:
	ldr	w0, [sp, 68]
	add	w0, w0, 1
	str	w0, [sp, 68]
.L178:
	ldr	x0, [sp, 24]
	ldr	w0, [x0]
	ldr	w1, [sp, 68]
	cmp	w1, w0
	blt	.L180
	ldr	x0, [sp, 24]
	str	wzr, [x0]
	str	wzr, [sp, 72]
	b	.L181
.L184:
	ldr	w1, [sp, 48]
	ldr	w0, [sp, 72]
	add	w0, w1, w0
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 56]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L182
	ldr	w1, [sp, 48]
	ldr	w0, [sp, 72]
	add	w0, w1, w0
	sxtw	x0, w0
	lsl	x0, x0, 3
	ldr	x1, [sp, 56]
	add	x0, x1, x0
	ldr	x0, [x0]
	b	.L183
.L182:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L183:
	mov	x3, x0
	ldr	x2, [sp, 16]
	ldr	x1, [sp, 24]
	ldr	x0, [sp, 32]
	bl	_ltng_arr_push
	ldr	w0, [sp, 72]
	add	w0, w0, 1
	str	w0, [sp, 72]
.L181:
	ldr	w1, [sp, 72]
	ldr	w0, [sp, 76]
	cmp	w1, w0
	blt	.L184
	nop
	nop
	ldp	x29, x30, [sp], 80
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE34:
	.size	_ltng_arr_slice, .-_ltng_arr_slice
	.align	2
	.type	_ltng_arr_free, %function
_ltng_arr_free:
.LFB35:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 24]
	str	w1, [sp, 20]
	ldr	x0, [sp, 24]
	cmp	x0, 0
	beq	.L191
	str	wzr, [sp, 44]
	b	.L188
.L190:
	ldrsw	x0, [sp, 44]
	lsl	x0, x0, 3
	ldr	x1, [sp, 24]
	add	x0, x1, x0
	ldr	x0, [x0]
	cmp	x0, 0
	beq	.L189
	ldrsw	x0, [sp, 44]
	lsl	x0, x0, 3
	ldr	x1, [sp, 24]
	add	x0, x1, x0
	ldr	x0, [x0]
	bl	free
.L189:
	ldr	w0, [sp, 44]
	add	w0, w0, 1
	str	w0, [sp, 44]
.L188:
	ldr	w1, [sp, 44]
	ldr	w0, [sp, 20]
	cmp	w1, w0
	blt	.L190
	ldr	x0, [sp, 24]
	bl	free
	b	.L185
.L191:
	nop
.L185:
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE35:
	.size	_ltng_arr_free, .-_ltng_arr_free
	.align	2
	.type	_ltng_idx2, %function
_ltng_idx2:
.LFB36:
	.cfi_startproc
	sub	sp, sp, #16
	.cfi_def_cfa_offset 16
	str	w0, [sp, 12]
	str	w1, [sp, 8]
	str	w2, [sp, 4]
	ldr	w1, [sp, 12]
	ldr	w0, [sp, 4]
	mul	w1, w1, w0
	ldr	w0, [sp, 8]
	add	w0, w1, w0
	add	sp, sp, 16
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE36:
	.size	_ltng_idx2, .-_ltng_idx2
	.align	2
	.type	_ltng_idx3, %function
_ltng_idx3:
.LFB37:
	.cfi_startproc
	sub	sp, sp, #32
	.cfi_def_cfa_offset 32
	str	w0, [sp, 28]
	str	w1, [sp, 24]
	str	w2, [sp, 20]
	str	w3, [sp, 16]
	str	w4, [sp, 12]
	ldr	w1, [sp, 28]
	ldr	w0, [sp, 16]
	mul	w1, w1, w0
	ldr	w0, [sp, 24]
	add	w1, w1, w0
	ldr	w0, [sp, 12]
	mul	w1, w1, w0
	ldr	w0, [sp, 20]
	add	w0, w1, w0
	add	sp, sp, 32
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE37:
	.size	_ltng_idx3, .-_ltng_idx3
	.align	2
	.type	_ltng_retbuf, %function
_ltng_retbuf:
.LFB38:
	.cfi_startproc
	adrp	x0, i.5
	add	x0, x0, :lo12:i.5
	ldr	w0, [x0]
	add	w0, w0, 1
	negs	w1, w0
	and	w0, w0, 31
	and	w1, w1, 31
	csneg	w1, w0, w1, mi
	adrp	x0, i.5
	add	x0, x0, :lo12:i.5
	str	w1, [x0]
	adrp	x0, i.5
	add	x0, x0, :lo12:i.5
	ldr	w2, [x0]
	adrp	x0, b.4
	add	x1, x0, :lo12:b.4
	sxtw	x0, w2
	lsl	x0, x0, 8
	add	x0, x1, x0
	strb	wzr, [x0]
	adrp	x0, i.5
	add	x0, x0, :lo12:i.5
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x1, x0, 8
	adrp	x0, b.4
	add	x0, x0, :lo12:b.4
	add	x0, x1, x0
	ret
	.cfi_endproc
.LFE38:
	.size	_ltng_retbuf, .-_ltng_retbuf
	.section	.rodata
	.align	3
.LC10:
	.string	"%g"
	.text
	.align	2
	.type	_ltng_ad, %function
_ltng_ad:
.LFB39:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	d0, [sp, 24]
	bl	_ltng_retbuf
	str	x0, [sp, 40]
	ldr	d0, [sp, 24]
	adrp	x0, .LC10
	add	x2, x0, :lo12:.LC10
	mov	x1, 256
	ldr	x0, [sp, 40]
	bl	snprintf
	ldr	x0, [sp, 40]
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE39:
	.size	_ltng_ad, .-_ltng_ad
	.align	2
	.type	_ltng_as, %function
_ltng_as:
.LFB40:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 24]
	bl	_ltng_retbuf
	str	x0, [sp, 40]
	ldr	x0, [sp, 24]
	cmp	x0, 0
	beq	.L201
	mov	x2, 255
	ldr	x1, [sp, 24]
	ldr	x0, [sp, 40]
	bl	strncpy
	ldr	x0, [sp, 40]
	add	x0, x0, 255
	strb	wzr, [x0]
.L201:
	ldr	x0, [sp, 40]
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE40:
	.size	_ltng_as, .-_ltng_as
	.section	.rodata
	.align	3
.LC11:
	.string	"%lld"
	.text
	.align	2
	.type	_ltng_from_int, %function
_ltng_from_int:
.LFB41:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	str	x1, [sp, 16]
	ldr	x3, [sp, 16]
	adrp	x0, .LC11
	add	x2, x0, :lo12:.LC11
	mov	x1, 256
	ldr	x0, [sp, 24]
	bl	snprintf
	ldr	x0, [sp, 24]
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE41:
	.size	_ltng_from_int, .-_ltng_from_int
	.align	2
	.type	_ltng_from_double, %function
_ltng_from_double:
.LFB42:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	str	d0, [sp, 16]
	ldr	d0, [sp, 16]
	adrp	x0, .LC10
	add	x2, x0, :lo12:.LC10
	mov	x1, 256
	ldr	x0, [sp, 24]
	bl	snprintf
	ldr	x0, [sp, 24]
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE42:
	.size	_ltng_from_double, .-_ltng_from_double
	.align	2
	.type	_ltng_is_num_str, %function
_ltng_is_num_str:
.LFB43:
	.cfi_startproc
	stp	x29, x30, [sp, -48]!
	.cfi_def_cfa_offset 48
	.cfi_offset 29, -48
	.cfi_offset 30, -40
	mov	x29, sp
	str	x0, [sp, 24]
	ldr	x0, [sp, 24]
	cmp	x0, 0
	beq	.L208
	ldr	x0, [sp, 24]
	ldrb	w0, [x0]
	cmp	w0, 0
	bne	.L209
.L208:
	mov	w0, 0
	b	.L210
.L209:
	ldr	x0, [sp, 24]
	str	x0, [sp, 40]
	ldr	x0, [sp, 40]
	ldrb	w0, [x0]
	cmp	w0, 45
	beq	.L211
	ldr	x0, [sp, 40]
	ldrb	w0, [x0]
	cmp	w0, 43
	bne	.L212
.L211:
	ldr	x0, [sp, 40]
	add	x0, x0, 1
	str	x0, [sp, 40]
.L212:
	str	wzr, [sp, 36]
	b	.L213
.L216:
	bl	__ctype_b_loc
	ldr	x1, [x0]
	ldr	x0, [sp, 40]
	ldrb	w0, [x0]
	and	x0, x0, 255
	lsl	x0, x0, 1
	add	x0, x1, x0
	ldrh	w0, [x0]
	and	w0, w0, 2048
	cmp	w0, 0
	beq	.L214
	mov	w0, 1
	str	w0, [sp, 36]
	b	.L215
.L214:
	ldr	x0, [sp, 40]
	ldrb	w0, [x0]
	cmp	w0, 46
	beq	.L215
	mov	w0, 0
	b	.L210
.L215:
	ldr	x0, [sp, 40]
	add	x0, x0, 1
	str	x0, [sp, 40]
.L213:
	ldr	x0, [sp, 40]
	ldrb	w0, [x0]
	cmp	w0, 0
	bne	.L216
	ldr	w0, [sp, 36]
.L210:
	ldp	x29, x30, [sp], 48
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE43:
	.size	_ltng_is_num_str, .-_ltng_is_num_str
	.section	.rodata
	.align	3
.LC12:
	.string	"%s%s"
	.text
	.align	2
	.type	_ltng_add_sn, %function
_ltng_add_sn:
.LFB44:
	.cfi_startproc
	stp	x29, x30, [sp, -64]!
	.cfi_def_cfa_offset 64
	.cfi_offset 29, -64
	.cfi_offset 30, -56
	mov	x29, sp
	str	x0, [sp, 24]
	str	x1, [sp, 16]
	adrp	x0, _ltng_asn_idx.3
	add	x0, x0, :lo12:_ltng_asn_idx.3
	ldr	w0, [x0]
	add	w0, w0, 1
	negs	w1, w0
	and	w0, w0, 31
	and	w1, w1, 31
	csneg	w1, w0, w1, mi
	adrp	x0, _ltng_asn_idx.3
	add	x0, x0, :lo12:_ltng_asn_idx.3
	str	w1, [x0]
	adrp	x0, _ltng_asn_idx.3
	add	x0, x0, :lo12:_ltng_asn_idx.3
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x1, x0, 8
	adrp	x0, buf.2
	add	x0, x0, :lo12:buf.2
	add	x0, x1, x0
	str	x0, [sp, 40]
	ldr	x0, [sp, 40]
	strb	wzr, [x0]
	ldr	x0, [sp, 24]
	bl	_ltng_is_num_str
	cmp	w0, 0
	beq	.L218
	ldr	x0, [sp, 16]
	bl	_ltng_is_num_str
	cmp	w0, 0
	beq	.L218
	ldr	x0, [sp, 24]
	bl	atof
	str	d0, [sp, 48]
	ldr	x0, [sp, 16]
	bl	atof
	str	d0, [sp, 56]
	ldr	d30, [sp, 48]
	ldr	d31, [sp, 56]
	fadd	d31, d30, d31
	fmov	d0, d31
	adrp	x0, .LC10
	add	x2, x0, :lo12:.LC10
	mov	x1, 256
	ldr	x0, [sp, 40]
	bl	snprintf
	b	.L219
.L218:
	ldr	x4, [sp, 16]
	ldr	x3, [sp, 24]
	adrp	x0, .LC12
	add	x2, x0, :lo12:.LC12
	mov	x1, 256
	ldr	x0, [sp, 40]
	bl	snprintf
.L219:
	ldr	x0, [sp, 40]
	ldp	x29, x30, [sp], 64
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE44:
	.size	_ltng_add_sn, .-_ltng_add_sn
	.section	.rodata
	.align	3
.LC13:
	.string	"%llu"
	.text
	.align	2
	.type	_ltng_ptr_to_str, %function
_ltng_ptr_to_str:
.LFB45:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	adrp	x0, i.1
	add	x0, x0, :lo12:i.1
	ldr	w0, [x0]
	add	w0, w0, 1
	negs	w1, w0
	and	w0, w0, 31
	and	w1, w1, 31
	csneg	w1, w0, w1, mi
	adrp	x0, i.1
	add	x0, x0, :lo12:i.1
	str	w1, [x0]
	adrp	x0, i.1
	add	x0, x0, :lo12:i.1
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x1, x0, 5
	adrp	x0, b.0
	add	x0, x0, :lo12:b.0
	add	x4, x1, x0
	ldr	x0, [sp, 24]
	mov	x3, x0
	adrp	x0, .LC13
	add	x2, x0, :lo12:.LC13
	mov	x1, 32
	mov	x0, x4
	bl	snprintf
	adrp	x0, i.1
	add	x0, x0, :lo12:i.1
	ldr	w0, [x0]
	sxtw	x0, w0
	lsl	x1, x0, 5
	adrp	x0, b.0
	add	x0, x0, :lo12:b.0
	add	x0, x1, x0
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE45:
	.size	_ltng_ptr_to_str, .-_ltng_ptr_to_str
	.align	2
	.type	_ltng_str_to_ptr, %function
_ltng_str_to_ptr:
.LFB46:
	.cfi_startproc
	stp	x29, x30, [sp, -32]!
	.cfi_def_cfa_offset 32
	.cfi_offset 29, -32
	.cfi_offset 30, -24
	mov	x29, sp
	str	x0, [sp, 24]
	ldr	x0, [sp, 24]
	cmp	x0, 0
	beq	.L224
	ldr	x0, [sp, 24]
	ldrb	w0, [x0]
	cmp	w0, 0
	bne	.L225
.L224:
	mov	x0, 0
	b	.L226
.L225:
	mov	w2, 10
	mov	x1, 0
	ldr	x0, [sp, 24]
	bl	__isoc23_strtoull
.L226:
	ldp	x29, x30, [sp], 32
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE46:
	.size	_ltng_str_to_ptr, .-_ltng_str_to_ptr
	.section	.rodata
	.align	3
.LC14:
	.string	"retval"
	.align	3
.LC15:
	.string	"try-return"
	.align	3
.LC16:
	.string	"value"
	.align	3
.LC17:
	.string	"finally-return"
	.text
	.align	2
	.type	retval, %function
retval:
.LFB47:
	.cfi_startproc
	stp	x29, x30, [sp, -16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	mov	x13, 12272
	sub	sp, sp, x13
	.cfi_def_cfa_offset 12288
	str	xzr, [sp, 1024]
	str	x0, [sp, 8]
	str	w1, [sp, 4]
	str	w2, [sp]
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x1, [x0]
	str	x1, [sp, 12264]
	mov	x1, 0
	ldr	x0, [sp, 8]
	cmp	x0, 0
	bne	.L228
	bl	_ltng_retbuf
	str	x0, [sp, 8]
.L228:
	ldr	x0, [sp, 8]
	strb	wzr, [x0]
	ldr	w2, [sp]
	ldr	w1, [sp, 4]
	adrp	x0, .LC14
	add	x0, x0, :lo12:.LC14
	bl	_ltng_fn_push
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	ldr	x0, [x0]
	str	x0, [sp, 336]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	str	w0, [sp, 344]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	add	x1, sp, 24
	str	x1, [x0]
	add	x0, sp, 24
	bl	_setjmp
	str	w0, [sp, 20]
	ldr	w0, [sp, 20]
	cmp	w0, 0
	bne	.L230
	adrp	x0, .LC15
	add	x0, x0, :lo12:.LC15
	bl	puts
	add	x0, sp, 8192
	add	x0, x0, 3816
	movi	v31.4s, 0
	str	q31, [x0]
	str	q31, [x0, 16]
	str	q31, [x0, 32]
	str	q31, [x0, 48]
	str	q31, [x0, 64]
	str	q31, [x0, 80]
	str	q31, [x0, 96]
	str	q31, [x0, 112]
	str	q31, [x0, 128]
	str	q31, [x0, 144]
	str	q31, [x0, 160]
	str	q31, [x0, 176]
	str	q31, [x0, 192]
	str	q31, [x0, 208]
	str	q31, [x0, 224]
	str	q31, [x0, 240]
	add	x4, sp, 8192
	add	x4, x4, 3816
	adrp	x0, .LC16
	add	x3, x0, :lo12:.LC16
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	mov	x0, x4
	bl	snprintf
	ldr	x1, [sp, 336]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	adrp	x0, .LC17
	add	x0, x0, :lo12:.LC17
	bl	puts
	bl	_ltng_fn_pop
	add	x0, sp, 8192
	add	x0, x0, 3816
	mov	x3, x0
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	ldr	x0, [sp, 8]
	bl	snprintf
	ldr	x0, [sp, 8]
	b	.L231
.L230:
	ldr	x1, [sp, 336]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	ldr	w1, [sp, 344]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	str	w1, [x0]
	add	x0, sp, 352
	bl	_ltng_exc_save_to
	adrp	x0, .LC17
	add	x0, x0, :lo12:.LC17
	bl	puts
	add	x0, sp, 352
	bl	_ltng_exc_restore_from
	bl	_ltng_rethrow
	bl	_ltng_fn_pop
	ldr	x0, [sp, 8]
.L231:
	mov	x1, x0
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x3, [sp, 12264]
	ldr	x2, [x0]
	subs	x3, x3, x2
	mov	x2, 0
	beq	.L232
	bl	__stack_chk_fail
.L232:
	mov	x0, x1
	mov	x13, 12272
	add	sp, sp, x13
	.cfi_def_cfa_offset 16
	ldp	x29, x30, [sp], 16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE47:
	.size	retval, .-retval
	.section	.rodata
	.align	3
.LC18:
	.string	"override"
	.align	3
.LC19:
	.string	"try"
	.align	3
.LC20:
	.string	"finally"
	.text
	.align	2
	.type	override, %function
override:
.LFB48:
	.cfi_startproc
	stp	x29, x30, [sp, -16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	mov	x13, 12272
	sub	sp, sp, x13
	.cfi_def_cfa_offset 12288
	str	xzr, [sp, 1024]
	str	x0, [sp, 8]
	str	w1, [sp, 4]
	str	w2, [sp]
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x1, [x0]
	str	x1, [sp, 12264]
	mov	x1, 0
	ldr	x0, [sp, 8]
	cmp	x0, 0
	bne	.L234
	bl	_ltng_retbuf
	str	x0, [sp, 8]
.L234:
	ldr	x0, [sp, 8]
	strb	wzr, [x0]
	ldr	w2, [sp]
	ldr	w1, [sp, 4]
	adrp	x0, .LC18
	add	x0, x0, :lo12:.LC18
	bl	_ltng_fn_push
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	ldr	x0, [x0]
	str	x0, [sp, 336]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	str	w0, [sp, 344]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	add	x1, sp, 24
	str	x1, [x0]
	add	x0, sp, 24
	bl	_setjmp
	str	w0, [sp, 20]
	ldr	w0, [sp, 20]
	cmp	w0, 0
	bne	.L236
	add	x0, sp, 8192
	add	x0, x0, 3816
	movi	v31.4s, 0
	str	q31, [x0]
	str	q31, [x0, 16]
	str	q31, [x0, 32]
	str	q31, [x0, 48]
	str	q31, [x0, 64]
	str	q31, [x0, 80]
	str	q31, [x0, 96]
	str	q31, [x0, 112]
	str	q31, [x0, 128]
	str	q31, [x0, 144]
	str	q31, [x0, 160]
	str	q31, [x0, 176]
	str	q31, [x0, 192]
	str	q31, [x0, 208]
	str	q31, [x0, 224]
	str	q31, [x0, 240]
	add	x4, sp, 8192
	add	x4, x4, 3816
	adrp	x0, .LC19
	add	x3, x0, :lo12:.LC19
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	mov	x0, x4
	bl	snprintf
	ldr	x1, [sp, 336]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	bl	_ltng_fn_pop
	adrp	x0, .LC20
	add	x3, x0, :lo12:.LC20
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	ldr	x0, [sp, 8]
	bl	snprintf
	ldr	x0, [sp, 8]
	b	.L238
.L236:
	ldr	x1, [sp, 336]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	ldr	w1, [sp, 344]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	str	w1, [x0]
	add	x0, sp, 352
	bl	_ltng_exc_save_to
	bl	_ltng_fn_pop
	adrp	x0, .LC20
	add	x3, x0, :lo12:.LC20
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	ldr	x0, [sp, 8]
	bl	snprintf
	ldr	x0, [sp, 8]
.L238:
	mov	x1, x0
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x3, [sp, 12264]
	ldr	x2, [x0]
	subs	x3, x3, x2
	mov	x2, 0
	beq	.L239
	bl	__stack_chk_fail
.L239:
	mov	x0, x1
	mov	x13, 12272
	add	sp, sp, x13
	.cfi_def_cfa_offset 16
	ldp	x29, x30, [sp], 16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE48:
	.size	override, .-override
	.section	.rodata
	.align	3
.LC22:
	.string	"Unknown function in indirect call"
	.text
	.align	2
	.type	__ltng_call_indirect, %function
__ltng_call_indirect:
.LFB49:
	.cfi_startproc
	stp	x29, x30, [sp, -176]!
	.cfi_def_cfa_offset 176
	.cfi_offset 29, -176
	.cfi_offset 30, -168
	mov	x29, sp
	sub	sp, sp, #944
	.cfi_def_cfa_offset 1120
	str	x0, [sp, 24]
	str	x1, [sp, 16]
	str	w2, [sp, 12]
	str	w3, [sp, 8]
	str	w4, [sp, 4]
	str	x5, [sp, 1096]
	str	x6, [sp, 1104]
	str	x7, [sp, 1112]
	add	x0, sp, 960
	str	q0, [x0]
	add	x0, sp, 976
	str	q1, [x0]
	add	x0, sp, 992
	str	q2, [x0]
	add	x0, sp, 1008
	str	q3, [x0]
	add	x0, sp, 1024
	str	q4, [x0]
	add	x0, sp, 1040
	str	q5, [x0]
	add	x0, sp, 1056
	str	q6, [x0]
	add	x0, sp, 1072
	str	q7, [x0]
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x1, [x0]
	str	x1, [sp, 936]
	mov	x1, 0
	adrp	x0, .LC21
	add	x1, x0, :lo12:.LC21
	add	x0, sp, 168
	mov	x3, x1
	mov	x1, 512
	mov	x2, x1
	mov	x1, x3
	bl	memcpy
	add	x0, sp, 1120
	str	x0, [sp, 72]
	add	x0, sp, 1120
	str	x0, [sp, 80]
	add	x0, sp, 1088
	str	x0, [sp, 88]
	mov	w0, -24
	str	w0, [sp, 96]
	mov	w0, -128
	str	w0, [sp, 100]
	str	wzr, [sp, 32]
	b	.L241
.L248:
	ldr	w1, [sp, 96]
	ldr	x0, [sp, 72]
	cmp	w1, 0
	blt	.L242
	add	x1, x0, 15
	and	x1, x1, -8
	str	x1, [sp, 72]
	b	.L243
.L242:
	add	w2, w1, 8
	str	w2, [sp, 96]
	ldr	w2, [sp, 96]
	cmp	w2, 0
	ble	.L244
	add	x1, x0, 15
	and	x1, x1, -8
	str	x1, [sp, 72]
	b	.L243
.L244:
	ldr	x2, [sp, 80]
	sxtw	x0, w1
	add	x0, x2, x0
.L243:
	ldr	x0, [x0]
	str	x0, [sp, 48]
	ldr	x0, [sp, 48]
	cmp	x0, 0
	beq	.L245
	ldr	x0, [sp, 48]
	b	.L246
.L245:
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
.L246:
	ldrsw	x1, [sp, 32]
	lsl	x1, x1, 3
	add	x2, sp, 168
	str	x0, [x2, x1]
	ldr	w0, [sp, 32]
	add	w0, w0, 1
	str	w0, [sp, 32]
.L241:
	ldr	w1, [sp, 32]
	ldr	w0, [sp, 4]
	cmp	w1, w0
	bge	.L247
	ldr	w0, [sp, 32]
	cmp	w0, 63
	ble	.L248
.L247:
	ldr	x0, [sp, 24]
	cmp	x0, 0
	bne	.L249
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 24]
.L249:
	add	x4, sp, 680
	ldr	x3, [sp, 24]
	adrp	x0, .LC4
	add	x2, x0, :lo12:.LC4
	mov	x1, 256
	mov	x0, x4
	bl	snprintf
	add	x0, sp, 104
	movi	v31.4s, 0
	str	q31, [x0]
	str	q31, [x0, 16]
	str	q31, [x0, 32]
	str	q31, [x0, 48]
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 104]
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 112]
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 120]
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 128]
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 136]
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 144]
	adrp	x0, .LC0
	add	x0, x0, :lo12:.LC0
	str	x0, [sp, 152]
	str	wzr, [sp, 36]
	add	x0, sp, 680
	mov	w1, 1
	bl	strchr
	str	x0, [sp, 56]
	ldr	x0, [sp, 56]
	cmp	x0, 0
	beq	.L250
	ldr	x0, [sp, 56]
	strb	wzr, [x0]
	ldr	x0, [sp, 56]
	add	x0, x0, 1
	str	x0, [sp, 40]
	b	.L251
.L253:
	mov	w1, 1
	ldr	x0, [sp, 40]
	bl	strchr
	str	x0, [sp, 64]
	ldr	x0, [sp, 64]
	cmp	x0, 0
	beq	.L252
	ldr	x0, [sp, 64]
	strb	wzr, [x0]
	ldr	w0, [sp, 36]
	add	w1, w0, 1
	str	w1, [sp, 36]
	sxtw	x0, w0
	lsl	x0, x0, 3
	add	x1, sp, 104
	ldr	x2, [sp, 40]
	str	x2, [x1, x0]
	ldr	x0, [sp, 64]
	add	x0, x0, 1
	str	x0, [sp, 40]
	b	.L251
.L252:
	ldr	w0, [sp, 36]
	add	w1, w0, 1
	str	w1, [sp, 36]
	sxtw	x0, w0
	lsl	x0, x0, 3
	add	x1, sp, 104
	ldr	x2, [sp, 40]
	str	x2, [x1, x0]
	b	.L250
.L251:
	ldr	x0, [sp, 40]
	cmp	x0, 0
	beq	.L250
	ldr	x0, [sp, 40]
	ldrb	w0, [x0]
	cmp	w0, 0
	beq	.L250
	ldr	w0, [sp, 36]
	cmp	w0, 7
	ble	.L253
.L250:
	add	x2, sp, 680
	adrp	x0, .LC14
	add	x1, x0, :lo12:.LC14
	mov	x0, x2
	bl	strcmp
	cmp	w0, 0
	bne	.L254
	ldr	w2, [sp, 8]
	ldr	w1, [sp, 12]
	ldr	x0, [sp, 16]
	bl	retval
	b	.L257
.L254:
	add	x2, sp, 680
	adrp	x0, .LC18
	add	x1, x0, :lo12:.LC18
	mov	x0, x2
	bl	strcmp
	cmp	w0, 0
	bne	.L256
	ldr	w2, [sp, 8]
	ldr	w1, [sp, 12]
	ldr	x0, [sp, 16]
	bl	override
	b	.L257
.L256:
	ldr	w3, [sp, 8]
	ldr	w2, [sp, 12]
	adrp	x0, .LC22
	add	x1, x0, :lo12:.LC22
	adrp	x0, .LC7
	add	x0, x0, :lo12:.LC7
	bl	_ltng_throw_value
	ldr	x0, [sp, 16]
.L257:
	mov	x1, x0
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x3, [sp, 936]
	ldr	x2, [x0]
	subs	x3, x3, x2
	mov	x2, 0
	beq	.L258
	bl	__stack_chk_fail
.L258:
	mov	x0, x1
	add	sp, sp, 944
	.cfi_def_cfa_offset 176
	ldp	x29, x30, [sp], 176
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE49:
	.size	__ltng_call_indirect, .-__ltng_call_indirect
	.data
	.align	3
.LC21:
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.xword	.LC0
	.text
	.section	.rodata
	.align	3
.LC23:
	.string	"main"
	.align	3
.LC24:
	.string	"fin "
	.align	3
.LC25:
	.string	"loop "
	.text
	.align	2
	.global	main
	.type	main, %function
main:
.LFB50:
	.cfi_startproc
	stp	x29, x30, [sp, -16]!
	.cfi_def_cfa_offset 16
	.cfi_offset 29, -16
	.cfi_offset 30, -8
	mov	x29, sp
	mov	x13, 14816
	sub	sp, sp, x13
	.cfi_def_cfa_offset 14832
	str	xzr, [sp, 1024]
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x1, [x0]
	str	x1, [sp, 14808]
	mov	x1, 0
	mov	w2, 1
	mov	w1, 1
	adrp	x0, .LC23
	add	x0, x0, :lo12:.LC23
	bl	_ltng_fn_push
	bl	_ltng_retbuf
	mov	w2, 7
	mov	w1, 9
	bl	retval
	bl	puts
	mov	x0, 48
	mov	x1, 0
	add	x2, sp, 8192
	add	x2, x2, 3800
	stp	x0, x1, [x2]
	add	x0, sp, 8192
	add	x0, x0, 3816
	movi	v31.4s, 0
	str	q31, [x0]
	str	q31, [x0, 16]
	str	q31, [x0, 32]
	str	q31, [x0, 48]
	str	q31, [x0, 64]
	str	q31, [x0, 80]
	str	q31, [x0, 96]
	str	q31, [x0, 112]
	str	q31, [x0, 128]
	str	q31, [x0, 144]
	str	q31, [x0, 160]
	str	q31, [x0, 176]
	str	q31, [x0, 192]
	str	q31, [x0, 208]
	str	q31, [x0, 224]
	add	x4, sp, 8192
	add	x4, x4, 3800
	mov	x3, 0
	adrp	x0, .LC11
	add	x2, x0, :lo12:.LC11
	mov	x1, 256
	mov	x0, x4
	bl	snprintf
	b	.L260
.L268:
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	ldr	x0, [x0]
	str	x0, [sp, 320]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	ldr	w0, [x0]
	str	w0, [sp, 328]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	add	x1, sp, 8
	str	x1, [x0]
	add	x0, sp, 8
	bl	_setjmp
	str	w0, [sp, 4]
	ldr	w0, [sp, 4]
	cmp	w0, 0
	bne	.L262
	add	x0, sp, 8192
	add	x0, x0, 3800
	bl	atoi
	cmp	w0, 1
	bne	.L263
	ldr	x1, [sp, 320]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	add	x0, sp, 8192
	add	x0, x0, 3800
	add	x5, sp, 8192
	add	x5, x5, 4056
	mov	x4, x0
	adrp	x0, .LC24
	add	x3, x0, :lo12:.LC24
	adrp	x0, .LC12
	add	x2, x0, :lo12:.LC12
	mov	x1, 512
	mov	x0, x5
	bl	snprintf
	add	x0, sp, 8192
	add	x0, x0, 4056
	bl	puts
	b	.L264
.L263:
	add	x0, sp, 8192
	add	x0, x0, 3800
	add	x5, sp, 12288
	add	x5, x5, 472
	mov	x4, x0
	adrp	x0, .LC25
	add	x3, x0, :lo12:.LC25
	adrp	x0, .LC12
	add	x2, x0, :lo12:.LC12
	mov	x1, 512
	mov	x0, x5
	bl	snprintf
	add	x0, sp, 12288
	add	x0, x0, 472
	bl	puts
	add	x0, sp, 8192
	add	x0, x0, 3800
	bl	atoi
	cmp	w0, 2
	bne	.L265
	ldr	x1, [sp, 320]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	add	x0, sp, 8192
	add	x0, x0, 3800
	add	x5, sp, 12288
	add	x5, x5, 984
	mov	x4, x0
	adrp	x0, .LC24
	add	x3, x0, :lo12:.LC24
	adrp	x0, .LC12
	add	x2, x0, :lo12:.LC12
	mov	x1, 512
	mov	x0, x5
	bl	snprintf
	add	x0, sp, 12288
	add	x0, x0, 984
	bl	puts
	b	.L266
.L265:
	ldr	x1, [sp, 320]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	add	x0, sp, 8192
	add	x0, x0, 3800
	add	x5, sp, 12288
	add	x5, x5, 1496
	mov	x4, x0
	adrp	x0, .LC24
	add	x3, x0, :lo12:.LC24
	adrp	x0, .LC12
	add	x2, x0, :lo12:.LC12
	mov	x1, 512
	mov	x0, x5
	bl	snprintf
	add	x0, sp, 12288
	add	x0, x0, 1496
	bl	puts
	b	.L264
.L262:
	ldr	x1, [sp, 320]
	adrp	x0, _ltng_try_top
	add	x0, x0, :lo12:_ltng_try_top
	str	x1, [x0]
	ldr	w1, [sp, 328]
	adrp	x0, _ltng_stack_depth
	add	x0, x0, :lo12:_ltng_stack_depth
	str	w1, [x0]
	add	x0, sp, 336
	bl	_ltng_exc_save_to
	add	x0, sp, 8192
	add	x0, x0, 3800
	add	x5, sp, 12288
	add	x5, x5, 2008
	mov	x4, x0
	adrp	x0, .LC24
	add	x3, x0, :lo12:.LC24
	adrp	x0, .LC12
	add	x2, x0, :lo12:.LC12
	mov	x1, 512
	mov	x0, x5
	bl	snprintf
	add	x0, sp, 12288
	add	x0, x0, 2008
	bl	puts
	add	x0, sp, 336
	bl	_ltng_exc_restore_from
	bl	_ltng_rethrow
.L264:
	add	x0, sp, 8192
	add	x0, x0, 3800
	bl	atoll
	add	x0, x0, 1
	add	x4, sp, 8192
	add	x4, x4, 3800
	mov	x3, x0
	adrp	x0, .LC11
	add	x2, x0, :lo12:.LC11
	mov	x1, 256
	mov	x0, x4
	bl	snprintf
.L260:
	add	x0, sp, 8192
	add	x0, x0, 3800
	bl	atoll
	cmp	x0, 3
	ble	.L268
.L266:
	bl	_ltng_retbuf
	mov	w2, 7
	mov	w1, 32
	bl	override
	bl	puts
	mov	w0, 0
	mov	w1, w0
	adrp	x0, :got:__stack_chk_guard
	ldr	x0, [x0, :got_lo12:__stack_chk_guard]
	ldr	x3, [sp, 14808]
	ldr	x2, [x0]
	subs	x3, x3, x2
	mov	x2, 0
	beq	.L270
	bl	__stack_chk_fail
.L270:
	mov	w0, w1
	mov	x13, 14816
	add	sp, sp, x13
	.cfi_def_cfa_offset 16
	ldp	x29, x30, [sp], 16
	.cfi_restore 30
	.cfi_restore 29
	.cfi_def_cfa_offset 0
	ret
	.cfi_endproc
.LFE50:
	.size	main, .-main
	.local	b.7
	.comm	b.7,256,8
	.local	b.6
	.comm	b.6,256,8
	.local	i.5
	.comm	i.5,4,4
	.local	b.4
	.comm	b.4,8192,8
	.local	_ltng_asn_idx.3
	.comm	_ltng_asn_idx.3,4,4
	.local	buf.2
	.comm	buf.2,8192,8
	.local	i.1
	.comm	i.1,4,4
	.local	b.0
	.comm	b.0,1024,8
	.ident	"GCC: (Ubuntu 15.2.0-16ubuntu1) 15.2.0"
	.section	.note.GNU-stack,"",@progbits
