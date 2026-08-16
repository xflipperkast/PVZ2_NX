	.text
	.align 2
	.global glu_ctor_probe
	.type glu_ctor_probe, %function
glu_ctor_probe:
	/* At the completed-constructor epilogue x19 is `this`. Preserve all
	 * caller-saved registers while installing the analytics ID. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x19
	bl	glu_services_created
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Instructions replaced at libPVZ2.so +0x2500ba0 through +0x2500bac. */
	ldp	x29, x30, [sp, #192]
	ldp	x20, x19, [sp, #176]
	ldp	x22, x21, [sp, #160]
	ldp	x24, x23, [sp, #144]
	adrp	x17, glu_ctor_continue
	ldr	x17, [x17, #:lo12:glu_ctor_continue]
	br	x17
	.size glu_ctor_probe, .-glu_ctor_probe

	.align 2
	.global title_state_probe
	.type title_state_probe, %function
title_state_probe:
	/* Preserve caller-saved integer/vector state around the diagnostic call. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x19
	mov	w1, w20
	bl	title_state_changed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Instructions replaced at libPVZ2.so +0x13e61dc through +0x13e61e8. */
	str	w20, [x19, #184]
	ldr	q0, [x9]
	mov	w9, #0x65
	strb	w8, [sp, #32]
	/* w8 is dead after the replaced strb and gets reloaded at +0x13e6200. */
	adrp	x8, title_state_continue
	ldr	x8, [x8, #:lo12:title_state_continue]
	br	x8
	.size title_state_probe, .-title_state_probe

	.align 2
	.global title_state6_exit_probe
	.type title_state6_exit_probe, %function
title_state6_exit_probe:
	/* The original four instructions select state 12 from w0 bit 0. Preserve
	 * that decision while C records it, then reproduce the original arm. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x19
	ldr	w1, [sp]
	bl	title_state6_exit_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* hook_arm64 replaced TBZ/MOV/MOV/BL, so replay the nonzero state-12
	 * transition here and otherwise resume at the first untouched instruction. */
	tbz	w0, #0, 1f
	mov	x0, x19
	mov	w1, #12
	adrp	x17, title_state_setter
	ldr	x17, [x17, #:lo12:title_state_setter]
	blr	x17
1:
	adrp	x17, title_state6_exit_continue
	ldr	x17, [x17, #:lo12:title_state6_exit_continue]
	br	x17
	.size title_state6_exit_probe, .-title_state6_exit_probe

	.align 2
	.global readiness_waiter_state1_probe
	.type readiness_waiter_state1_probe, %function
readiness_waiter_state1_probe:
	/* Replaces +14fe778: TBZ / LDRB / CBZ / ADRP.  Record the result of
	 * +1791564, then reproduce its two original branch decisions. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #1
	mov	x1, x19
	ldr	w2, [sp]
	bl	readiness_waiter_predicate_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	tbz	w0, #0, 1f
	ldrb	w8, [x19, #146]
	cbz	w8, 2f
	adrp	x8, readiness_waiter_input_global_page
	ldr	x8, [x8, #:lo12:readiness_waiter_input_global_page]
	adrp	x17, readiness_waiter_state1_after_global
	ldr	x17, [x17, #:lo12:readiness_waiter_state1_after_global]
	br	x17
1:
	adrp	x17, readiness_waiter_state1_exit
	ldr	x17, [x17, #:lo12:readiness_waiter_state1_exit]
	br	x17
2:
	adrp	x17, readiness_waiter_state1_after_input
	ldr	x17, [x17, #:lo12:readiness_waiter_state1_after_input]
	br	x17
	.size readiness_waiter_state1_probe, .-readiness_waiter_state1_probe

	.align 2
	.global readiness_waiter_state2_probe
	.type readiness_waiter_state2_probe, %function
readiness_waiter_state2_probe:
	/* Replaces +14fe798: TBZ / LDR / BL / STRB.  The native +179361c call
	 * is replayed exactly when the optional predicate passed. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #2
	mov	x1, x19
	ldr	w2, [sp]
	bl	readiness_waiter_predicate_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	tbz	w0, #0, 1f
	ldr	x0, [x19, #16]
	adrp	x17, readiness_waiter_state2_helper
	ldr	x17, [x17, #:lo12:readiness_waiter_state2_helper]
	blr	x17
	strb	wzr, [x19, #184]
	adrp	x17, readiness_waiter_state2_after_helper
	ldr	x17, [x17, #:lo12:readiness_waiter_state2_after_helper]
	br	x17
1:
	adrp	x17, readiness_waiter_state2_exit
	ldr	x17, [x17, #:lo12:readiness_waiter_state2_exit]
	br	x17
	.size readiness_waiter_state2_probe, .-readiness_waiter_state2_probe

	.align 2
	.global readiness_waiter_state3_probe
	.type readiness_waiter_state3_probe, %function
readiness_waiter_state3_probe:
	/* Replaces +14fe7b4: TBZ / STR / CBZ / ADD.  A one here is the real
	 * completion path that clears waiter+8; this probe never changes it. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #3
	mov	x1, x19
	ldr	w2, [sp]
	bl	readiness_waiter_predicate_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	tbz	w0, #0, 1f
	str	wzr, [x19, #8]
	cbz	x20, 2f
	add	x1, x20, #0x28
	adrp	x17, readiness_waiter_state3_continue
	ldr	x17, [x17, #:lo12:readiness_waiter_state3_continue]
	br	x17
1:
	adrp	x17, readiness_waiter_state3_failure
	ldr	x17, [x17, #:lo12:readiness_waiter_state3_failure]
	br	x17
2:
	adrp	x17, readiness_waiter_state3_no_detail
	ldr	x17, [x17, #:lo12:readiness_waiter_state3_no_detail]
	br	x17
	.size readiness_waiter_state3_probe, .-readiness_waiter_state3_probe

	.align 2
	.global purchase_driver_probe
	.type purchase_driver_probe, %function
purchase_driver_probe:
	/* Preserve the constructor's volatile state while capturing x0 (this). */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	bl	purchase_driver_created
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Instructions replaced at libPVZ2.so +0x240a200 through +0x240a20c. */
	ldr	x8, [x23, #40]
	mov	x21, x0
	movi	v0.2d, #0
	stur	x8, [x29, #-8]
	adrp	x8, purchase_driver_continue
	ldr	x8, [x8, #:lo12:purchase_driver_continue]
	br	x8
	.size purchase_driver_probe, .-purchase_driver_probe

	.align 2
	.global diagnostic_state_machine_entry_probe
	.type diagnostic_state_machine_entry_probe, %function
diagnostic_state_machine_entry_probe:
	/* Hooked over libPVZ2 +0x20e966c..+0x20e9678.  Preserve the complete
	 * volatile ABI state while recording the holder/caller, then replay the
	 * exact native prologue and continue at +0x20e967c. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	ldr	x0, [sp]
	ldr	x1, [sp, #152]
	bl	diagnostic_state_machine_entered
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Original +0x20e966c .. +0x20e9678. */
	sub	sp, sp, #0x70
	stp	x29, x30, [sp, #0x50]
	stp	x20, x19, [sp, #0x60]
	add	x29, sp, #0x50
	adrp	x17, diagnostic_state_machine_continue
	ldr	x17, [x17, #:lo12:diagnostic_state_machine_continue]
	br	x17
	.size diagnostic_state_machine_entry_probe, .-diagnostic_state_machine_entry_probe

	.align 2
	.global diagnostic_state70_writer_probe
	.type diagnostic_state70_writer_probe, %function
diagnostic_state70_writer_probe:
	/* Surgical probe for libPVZ2 +0x2104bf4.
	 * Original block:
	 *   str w21, [x19, #0x70]
	 *   tbz w20, #0, +0x2104c54
	 *   cmp w8, #3
	 *   b.eq +0x2104c54
	 *
	 * Record the candidate object's old/new state without modifying it, then
	 * replay the exact native store and control flow. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]

	mov	x0, x19
	mov	w1, w21
	mov	w2, w20
	mov	w3, w8
	mov	x4, x29
	add	x5, sp, #288
	bl	diagnostic_state70_writer_observed

	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Replay +0x2104bf4..+0x2104c00 exactly. */
	str	w21, [x19, #0x70]
	tbz	w20, #0, .Ldiagnostic_writer_branch
	cmp	w8, #3
	b.eq	.Ldiagnostic_writer_branch
	adrp	x17, diagnostic_state70_writer_continue
	ldr	x17, [x17, #:lo12:diagnostic_state70_writer_continue]
	br	x17

.Ldiagnostic_writer_branch:
	adrp	x17, diagnostic_state70_writer_branch_target
	ldr	x17, [x17, #:lo12:diagnostic_state70_writer_branch_target]
	br	x17
	.size diagnostic_state70_writer_probe, .-diagnostic_state70_writer_probe

	/* Observe the return from the native request dispatcher called at
	 * +0x10147ac. The original BL is untouched, so the dispatcher sees the
	 * exact native LR. This hook starts at its real return address +0x10147b0. */
	.align 2
	.global diagnostic_dispatch_return_probe
	.type diagnostic_dispatch_return_probe, %function
diagnostic_dispatch_return_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x23
	ldr	x1, [sp]
	mov	x2, x29
	add	x3, sp, #288
	ldr	x4, [sp, #152]
	bl	diagnostic_request_dispatch_returned
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Original +0x10147b0..+0x10147bc. */
	ldr	x20, [sp, #0x88]
	adrp	x8, diagnostic_dispatch_adrp_value
	ldr	x8, [x8, #:lo12:diagnostic_dispatch_adrp_value]
	add	x8, x8, #0xdb8
	str	x8, [sp, #0x78]
	adrp	x17, diagnostic_dispatch_return_continue
	ldr	x17, [x17, #:lo12:diagnostic_dispatch_return_continue]
	br	x17
	.size diagnostic_dispatch_return_probe, .-diagnostic_dispatch_return_probe


	/* The non-empty main_experiment path reaches +0x1014850 and then
	 * calls the native Nimble request adapter at +0x23F474C. Observe its arguments,
	 * replay their setup, then tail-call the restored native adapter with the
	 * exact LR its original BL would have produced (+0x1014860). */
	.align 2
	.global diagnostic_nimble_request_call_probe
	.type diagnostic_nimble_request_call_probe, %function
diagnostic_nimble_request_call_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x23
	ldr	x1, [sp]
	sub	x2, x29, #0x38
	add	x3, sp, #(288 + 0x50)
	add	x4, sp, #(288 + 0x38)
	mov	x5, x29
	add	x6, sp, #288
	bl	diagnostic_nimble_request_adapter_entered
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Replay +0x1014850..+0x1014858, then reproduce the BL to +0x23F474C. */
	sub	x1, x29, #0x38
	add	x2, sp, #0x50
	add	x3, sp, #0x38
	adrp	x30, diagnostic_nimble_request_return
	ldr	x30, [x30, #:lo12:diagnostic_nimble_request_return]
	adrp	x17, diagnostic_nimble_request_adapter
	ldr	x17, [x17, #:lo12:diagnostic_nimble_request_adapter]
	br	x17
	.size diagnostic_nimble_request_call_probe, .-diagnostic_nimble_request_call_probe


	/* Native Nimble C++ component registrations must be preserved. By the time
	 * Message::getComponent reaches this exact
	 * lookup call, its libc++ key already exists at sp and the temporary
	 * shared_ptr result belongs at sp+0x18.  Ask the Switch registry for the
	 * retained native component, replay the displaced LDP, then rejoin the
	 * original helper.  The helper itself still applies its native +0x18
	 * interface adjustment and shared_ptr ownership transfer. */
	.align 2
	.global nimble_message_registry_lookup_probe
	.type nimble_message_registry_lookup_probe, %function
nimble_message_registry_lookup_probe:
	add	x0, sp, #0x18
	mov	x1, sp
	bl	nimble_cpp_component_lookup
	ldp	x8, x9, [sp, #0x18]
	adrp	x17, nimble_message_registry_lookup_continue
	ldr	x17, [x17, #:lo12:nimble_message_registry_lookup_continue]
	br	x17
	.size nimble_message_registry_lookup_probe, .-nimble_message_registry_lookup_probe


	/* NimbleCppComponentManager::getComponent is a static C++ function that
	 * returns shared_ptr through AArch64's hidden x8 result register and takes
	 * the libc++ name in x0.  Adapt that ABI to the Switch registry helper so
	 * every native consumer (including Nexus EAAccount) sees the components
	 * retained by nimble_cpp_component_register. */
	.align 2
	.global nimble_cpp_component_get
	.type nimble_cpp_component_get, %function
nimble_cpp_component_get:
	mov	x1, x0
	mov	x0, x8
	b	nimble_cpp_component_lookup
	.size nimble_cpp_component_get, .-nimble_cpp_component_get


	/* Call the unpatched component manager from C. The manager has C++'s
	 * hidden shared_ptr result in x8, whereas the compatibility caller uses
	 * the ordinary (result, name) pair in x0/x1. */
	.align 2
	.global nimble_cpp_component_native_lookup
	.type nimble_cpp_component_native_lookup, %function
nimble_cpp_component_native_lookup:
	adrp	x16, nimble_cpp_component_native_get
	ldr	x16, [x16, #:lo12:nimble_cpp_component_native_get]
	cbz	x16, 1f
	mov	x8, x0
	mov	x0, x1
	br	x16
1:
	stp	xzr, xzr, [x0]
	ret
	.size nimble_cpp_component_native_lookup, .-nimble_cpp_component_native_lookup


	/* +0x23F474C obtains the Android Nimble Message component into x19.
	 * When that result is null on Horizon, +0x23F4868 then
	 * dereferences it.  Observe the component.  If it is valid, replay the four
	 * displaced instructions and continue native execution.  If it is null,
	 * skip only the impossible virtual call and join the adapter's own cleanup
	 * at +0x23F4888.  No request/loader state is written here. */
	.align 2
	.global nimble_message_component_guard_probe
	.type nimble_message_component_guard_probe, %function
nimble_message_component_guard_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	mov	x3, x22
	mov	x4, x29
	add	x5, sp, #288
	bl	nimble_message_component_guard_observed
	/* Preserve the C probe's safe/unsafe verdict in NZCV while restoring all
	 * displaced native registers; LDP/ADD below do not modify flags. */
	cmp	w0, #0
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	b.eq	1f
	/* Replay +0x23F4868..+0x23F4874 only for a validated native proxy. */
	ldr	x8, [x19]
	ldr	x8, [x8, #0x18]
	add	x1, sp, #0x28
	sub	x2, x29, #0x30
	adrp	x17, nimble_message_component_continue
	ldr	x17, [x17, #:lo12:nimble_message_component_continue]
	br	x17
1:
	adrp	x17, nimble_message_component_null_cleanup
	ldr	x17, [x17, #:lo12:nimble_message_component_null_cleanup]
	br	x17
	.size nimble_message_component_guard_probe, .-nimble_message_component_guard_probe


	/* +0x1DA8450 reads PVZDB ForceUpdateConfigData; +0x40 is
	 * MinJoustVersion.  The Horizon port does not depend on the Android
	 * online bit, so preserve the version comparison when the record exists
	 * and treat a missing record as "no minimum Joust version". */
	.align 2
	.global joust_status_guard_probe
	.type joust_status_guard_probe, %function
joust_status_guard_probe:
	adrp	x17, joust_force_update_lookup
	ldr	x17, [x17, #:lo12:joust_force_update_lookup]
	blr	x17
	cbz	x0, 1f
	add	x1, x0, #0x40
	mov	x0, sp
	adrp	x17, joust_version_string_copy
	ldr	x17, [x17, #:lo12:joust_version_string_copy]
	br	x17
1:
	/* +0x1DA8480 reloads the Joust UI vfunc and then tests w20.  Clearing
	 * w20 selects its existing state-0 (JoustAvailableButtonContainer) path. */
	mov	w20, wzr
	adrp	x17, joust_available_rejoin
	ldr	x17, [x17, #:lo12:joust_available_rejoin]
	br	x17
	.size joust_status_guard_probe, .-joust_status_guard_probe


	.align 2



	.align 2
	.global diagnostic_request_state2_probe
	.type diagnostic_request_state2_probe, %function
diagnostic_request_state2_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #2
	mov	x1, x19
	ldr	w2, [sp, #64]
	mov	x3, x29
	add	x4, sp, #288
	ldr	x5, [sp, #152]
	bl	diagnostic_request_lifecycle_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	str	w8, [x19, #0x70]
	ldr	x1, [x20]
	ldp	x28, x8, [x1, #0x18]
	cmp	x28, x8
	adrp	x17, diagnostic_request_state2_continue
	ldr	x17, [x17, #:lo12:diagnostic_request_state2_continue]
	br	x17
	.size diagnostic_request_state2_probe, .-diagnostic_request_state2_probe

	.align 2
	.global diagnostic_request_state5_probe
	.type diagnostic_request_state5_probe, %function
diagnostic_request_state5_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #5
	mov	x1, x19
	ldr	w2, [sp, #64]
	mov	x3, x29
	add	x4, sp, #288
	ldr	x5, [sp, #152]
	bl	diagnostic_request_lifecycle_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Original +0x1015288..+0x1015294. Tail into the native helper with the
	 * same LR (+0x1015298) that its original BL would have produced. */
	str	w8, [x19, #0x70]
	ldur	x23, [x29, #-0x88]
	mov	x0, x23
	adrp	x30, diagnostic_request_state5_continue
	ldr	x30, [x30, #:lo12:diagnostic_request_state5_continue]
	adrp	x17, diagnostic_request_state5_helper
	ldr	x17, [x17, #:lo12:diagnostic_request_state5_helper]
	br	x17
	.size diagnostic_request_state5_probe, .-diagnostic_request_state5_probe

	.align 2
	.global diagnostic_request_state6_probe
	.type diagnostic_request_state6_probe, %function
diagnostic_request_state6_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #6
	mov	x1, x19
	ldr	w2, [sp, #72]
	mov	x3, x29
	add	x4, sp, #288
	ldr	x5, [sp, #152]
	bl	diagnostic_request_lifecycle_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	str	w9, [x19, #0x70]
	mov	x0, x20
	ldr	x8, [x20]
	ldr	x8, [x8, #0x18]
	adrp	x17, diagnostic_request_state6_continue
	ldr	x17, [x17, #:lo12:diagnostic_request_state6_continue]
	br	x17
	.size diagnostic_request_state6_probe, .-diagnostic_request_state6_probe

	.align 2
	.global diagnostic_request_state4_probe
	.type diagnostic_request_state4_probe, %function
diagnostic_request_state4_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #4
	mov	x1, x19
	ldr	w2, [sp, #72]
	mov	x3, x29
	add	x4, sp, #288
	ldr	x5, [sp, #152]
	bl	diagnostic_request_lifecycle_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	str	w9, [x19, #0x70]
	mov	x0, x20
	ldr	x8, [x20]
	ldr	x8, [x8, #0x18]
	adrp	x17, diagnostic_request_state4_continue
	ldr	x17, [x17, #:lo12:diagnostic_request_state4_continue]
	br	x17
	.size diagnostic_request_state4_probe, .-diagnostic_request_state4_probe

	.align 2
	.global diagnostic_request_state3_probe
	.type diagnostic_request_state3_probe, %function
diagnostic_request_state3_probe:
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	w0, #3
	mov	x1, x27
	ldr	w2, [sp, #64]
	mov	x3, x29
	add	x4, sp, #288
	ldr	x5, [sp, #152]
	bl	diagnostic_request_lifecycle_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	str	w8, [x27, #0x70]
	ldur	x20, [x29, #-0x60]
	cbz	x20, .Ldiagnostic_state3_branch
	add	x1, x20, #0x8
	adrp	x17, diagnostic_request_state3_continue
	ldr	x17, [x17, #:lo12:diagnostic_request_state3_continue]
	br	x17
.Ldiagnostic_state3_branch:
	adrp	x17, diagnostic_request_state3_branch
	ldr	x17, [x17, #:lo12:diagnostic_request_state3_branch]
	br	x17
	.size diagnostic_request_state3_probe, .-diagnostic_request_state3_probe

	.align 2
	.global input_state_request_probe
	.type input_state_request_probe, %function
input_state_request_probe:
	/* Input-state diagnostic at libPVZ2 +0x150E3C0. Entry ABI is x0=state
	 * object, w1=requested state, x30=caller. Preserve caller-saved integer and
	 * SIMD state, report the request, then replay the exact four overwritten
	 * prologue instructions and resume at +0x150E3D0. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x2, x30
	bl	input_state_request_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Original +0x150E3C0..+0x150E3CC. */
	sub	sp, sp, #0xb0
	stp	x29, x30, [sp, #0x60]
	stp	x26, x25, [sp, #0x70]
	stp	x24, x23, [sp, #0x80]
	adrp	x17, input_state_request_continue
	ldr	x17, [x17, #:lo12:input_state_request_continue]
	br	x17
	.size input_state_request_probe, .-input_state_request_probe

	.align 2
	.global state5_driver_entry_probe
	.type state5_driver_entry_probe, %function
state5_driver_entry_probe:
	/* Read-only probe at corrected +0x151CC28. Preserve volatile state, report
	 * x0 object and original caller LR, then replay overwritten prologue. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x1, x30
	mov	w2, #0
	bl	state5_driver_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	/* Original +0x151CC28..+0x151CC34. */
	stp	x29, x30, [sp, #-0x20]!
	stp	x20, x19, [sp, #0x10]
	mov	x29, sp
	adrp	x20, state5_driver_entry_x20
	ldr	x20, [x20, #:lo12:state5_driver_entry_x20]
	adrp	x17, state5_driver_entry_continue
	ldr	x17, [x17, #:lo12:state5_driver_entry_continue]
	br	x17
	.size state5_driver_entry_probe, .-state5_driver_entry_probe

	.align 2
	.global state5_driver_block_probe
	.type state5_driver_block_probe, %function
state5_driver_block_probe:
	/* Read-only probe at +0x151CC70. x19 is the board object here. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x19
	mov	x1, x30
	mov	w2, #1
	bl	state5_driver_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288
	/* Original +0x151CC70..+0x151CC7C. */
	ldr	w8, [x19, #0xe0]
	ldr	s0, [x0, #0x24]
	sub	w8, w8, #3
	cmp	w8, #1
	adrp	x17, state5_driver_block_continue
	ldr	x17, [x17, #:lo12:state5_driver_block_continue]
	br	x17
	.size state5_driver_block_probe, .-state5_driver_block_probe


	.align 2
	.global state5_tail_gate_probe
	.type state5_tail_gate_probe, %function
state5_tail_gate_probe:
	/* Read-only probe at +0x150E8A8. At this point x19 is the board
	 * object and x20 is *(board+0x3D0). Preserve all volatile state, report
	 * the gate object, then replay the exact original STR/LDRB/CBNZ/MOV
	 * sequence. The original nonzero gate still branches to +0x150E3F4;
	 * the zero gate still continues to the real BL +0x151CC28. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x19
	mov	x1, x20
	bl	state5_tail_gate_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Original +0x150E8A8..+0x150E8B4. */
	str	wzr, [x8, #0x888]
	ldrb	w8, [x20, #0x70]
	cbnz	w8, 1f
	mov	x0, x19
	adrp	x17, state5_tail_gate_call_continue
	ldr	x17, [x17, #:lo12:state5_tail_gate_call_continue]
	br	x17
1:
	adrp	x17, state5_tail_gate_skip_continue
	ldr	x17, [x17, #:lo12:state5_tail_gate_skip_continue]
	br	x17
	.size state5_tail_gate_probe, .-state5_tail_gate_probe


	.align 2
	.global level_module_ctor_complete_probe
	.type level_module_ctor_complete_probe, %function
level_module_ctor_complete_probe:
	/* Read-only lifecycle probe at +0x14F60F4. Execute the original
	 * constructor's native zero store first. At this point x19=this+0x60,
	 * so the store is exactly manager+0x70=0. Then preserve volatile state,
	 * report the genuine constructed instance, and replay the remaining three
	 * overwritten epilogue instructions before the native RET. */
	strb	wzr, [x19, #0x10]

	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	sub	x0, x19, #0x60
	mov	x1, x30
	bl	level_module_ctor_complete_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Remaining original +0x14F60F8..+0x14F6100. +0x14F6104 is RET. */
	ldp	x20, x19, [sp, #0x20]
	ldr	x21, [sp, #0x10]
	ldp	x29, x30, [sp], #0x30
	ret
	.size level_module_ctor_complete_probe, .-level_module_ctor_complete_probe

	.align 2
	.global level_module_init_call_probe
	.type level_module_init_call_probe, %function
level_module_init_call_probe:
	/* Read-only wrapper for the genuine board construction callsite
	 * +0x151C408. The overwritten native window is:
	 *   BL   +0x14F68DC
	 *   SUB  X0,X29,#0x10
	 *   BL   +0x22617DC
	 *   LDR  W0,[X19,#0x17C]
	 * We observe manager+0x70 before and after the real +0x14F68DC call,
	 * preserve the call's returned volatile state across logging, replay the
	 * remaining three instructions, and continue at +0x151C418. */

	/* Pre-call observation: preserve the exact native call inputs. */
	sub	sp, sp, #320
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mrs	x9, nzcv
	mrs	x10, fpcr
	mrs	x11, fpsr
	str	x9, [sp, #288]
	str	x10, [sp, #296]
	str	x11, [sp, #304]
	mov	x0, x21
	mov	x1, x19
	mov	w2, #0
	bl	level_module_init_call_observed
	ldr	x9, [sp, #288]
	ldr	x10, [sp, #296]
	ldr	x11, [sp, #304]
	msr	nzcv, x9
	msr	fpcr, x10
	msr	fpsr, x11
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #320

	/* Execute the original BL +0x14F68DC. X17 is the ABI-designated IP
	 * scratch register and hook_arm64 already uses it for the entry veneer. */
	adrp	x17, level_module_init_call_target
	ldr	x17, [x17, #:lo12:level_module_init_call_target]
	blr	x17

	/* Post-call observation: preserve exactly what the native call returned. */
	sub	sp, sp, #320
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mrs	x9, nzcv
	mrs	x10, fpcr
	mrs	x11, fpsr
	str	x9, [sp, #288]
	str	x10, [sp, #296]
	str	x11, [sp, #304]
	mov	x0, x21
	mov	x1, x19
	mov	w2, #1
	bl	level_module_init_call_observed
	ldr	x9, [sp, #288]
	ldr	x10, [sp, #296]
	ldr	x11, [sp, #304]
	msr	nzcv, x9
	msr	fpcr, x10
	msr	fpsr, x11
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #320

	/* Replay original +0x151C40C..+0x151C414. */
	sub	x0, x29, #0x10
	adrp	x17, level_module_init_stack_helper
	ldr	x17, [x17, #:lo12:level_module_init_stack_helper]
	blr	x17
	ldr	w0, [x19, #0x17c]

	adrp	x17, level_module_init_call_continue
	ldr	x17, [x17, #:lo12:level_module_init_call_continue]
	br	x17
	.size level_module_init_call_probe, .-level_module_init_call_probe

	.align 2
	.global level_module_factory_return_probe
	.type level_module_factory_return_probe, %function
level_module_factory_return_probe:
	/* Read-only probe replacing +0x14F5EBC..+0x14F5EC8. x19 is
	 * the returned LevelModuleManager and the factory's saved native caller LR
	 * is at [original sp,#8]. Preserve volatile state while reporting it, then
	 * replay mov x0,x19 plus the exact native epilogue. */
	sub	sp, sp, #288
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mov	x0, x19
	ldr	x1, [sp, #296]
	bl	level_module_factory_return_observed
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #288

	/* Original +0x14F5EBC..+0x14F5EC8. */
	mov	x0, x19
	ldp	x20, x19, [sp, #0x10]
	ldp	x29, x30, [sp], #0x20
	ret
	.size level_module_factory_return_probe, .-level_module_factory_return_probe

	.align 2
	.global level_module_gate_store_probe
	.type level_module_gate_store_probe, %function
level_module_gate_store_probe:
	/* Read-only observation at +0x14FC508. hook_arm64 replaces a
	 * 16-byte native window beginning with STR W8,[X21,#0x70]. Save volatile
	 * state and NZCV, report the native intended store, restore everything,
	 * then preserve X17 across the branch into a relocation-checked trampoline.
	 * The trampoline restores X17, replays the exact native 16 bytes and
	 * branches directly back to +0x14FC518. */
	sub	sp, sp, #320
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mrs	x9, nzcv
	mrs	x10, fpcr
	mrs	x11, fpsr
	str	x9, [sp, #288]
	str	x10, [sp, #296]
	str	x11, [sp, #304]

	mov	x0, x21
	mov	w1, w8
	mov	x2, x30
	mov	x3, x19
	mov	x4, x20
	bl	level_module_gate_store_observed

	ldr	x9, [sp, #288]
	ldr	x10, [sp, #296]
	ldr	x11, [sp, #304]
	msr	nzcv, x9
	msr	fpcr, x10
	msr	fpsr, x11
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #320

	/* We need one branch scratch register, but X17 may be live at this exact
	 * mid-function point. Save it for the trampoline to restore before native
	 * instructions execute. */
	sub	sp, sp, #16
	str	x17, [sp]
	adrp	x17, level_module_gate_store_trampoline
	ldr	x17, [x17, #:lo12:level_module_gate_store_trampoline]
	br	x17
	.size level_module_gate_store_probe, .-level_module_gate_store_probe

	.align 2
	.global module_handle_trace_active_find_probe
	.type module_handle_trace_active_find_probe, %function
module_handle_trace_active_find_probe:
	/* Read-only observation at +0x14F64D8. At this point the native
	 * LevelModuleManager::find-style loop has already resolved the current
	 * generational handle to the real object in X21. X19=manager, X20=query,
	 * X25=active-handle index. Preserve all caller-clobbered state, report the
	 * mapping, then replay exactly +0x14F64D8..+0x14F64E4 and resume at the
	 * native BLR X8 (+0x14F64E8). */
	sub	sp, sp, #320
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mrs	x9, nzcv
	mrs	x10, fpcr
	mrs	x11, fpsr
	str	x9, [sp, #288]
	str	x10, [sp, #296]
	str	x11, [sp, #304]

	mov	x0, x21
	mov	x1, x19
	mov	x2, x25
	mov	w3, #0
	bl	module_handle_trace_active_handle_resolved_observed

	ldr	x9, [sp, #288]
	ldr	x10, [sp, #296]
	ldr	x11, [sp, #304]
	msr	nzcv, x9
	msr	fpcr, x10
	msr	fpsr, x11
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #320

	/* Exact original +0x14F64D8..+0x14F64E4. */
	ldr	x8, [x21]
	mov	x0, x21
	mov	x1, x20
	ldr	x8, [x8, #0x20]
	adrp	x17, module_handle_trace_active_find_continue
	ldr	x17, [x17, #:lo12:module_handle_trace_active_find_continue]
	br	x17
	.size module_handle_trace_active_find_probe, .-module_handle_trace_active_find_probe

	.align 2
	.global module_handle_trace_active_collect_probe
	.type module_handle_trace_active_collect_probe, %function
module_handle_trace_active_collect_probe:
	/* Read-only observation at +0x14F669C. The collect-style manager
	 * loop has already resolved the current active handle to X23. X21=manager,
	 * X25=active-handle index. Observe it, replay exactly
	 * +0x14F669C..+0x14F66A8, then resume at native BLR X8 (+0x14F66AC). */
	sub	sp, sp, #320
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mrs	x9, nzcv
	mrs	x10, fpcr
	mrs	x11, fpsr
	str	x9, [sp, #288]
	str	x10, [sp, #296]
	str	x11, [sp, #304]

	mov	x0, x23
	mov	x1, x21
	mov	x2, x25
	mov	w3, #1
	bl	module_handle_trace_active_handle_resolved_observed

	ldr	x9, [sp, #288]
	ldr	x10, [sp, #296]
	ldr	x11, [sp, #304]
	msr	nzcv, x9
	msr	fpcr, x10
	msr	fpsr, x11
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #320

	/* Exact original +0x14F669C..+0x14F66A8. */
	ldr	x8, [x23]
	ldr	x8, [x8, #0x20]
	mov	x0, x23
	ldr	x1, [sp, #0x8]
	adrp	x17, module_handle_trace_active_collect_continue
	ldr	x17, [x17, #:lo12:module_handle_trace_active_collect_continue]
	br	x17
	.size module_handle_trace_active_collect_probe, .-module_handle_trace_active_collect_probe

	.align 2
	.global account_email_bridge_probe
	.type account_email_bridge_probe, %function
account_email_bridge_probe:
	/* Account safety bridge at libPVZ2 +0x23F7700.
	 * The nearby +0x23F768C location is inside an active
	 * stack frame.  Here we replace exactly the four instructions beginning at
	 * the proven x20 null dereference.  If the Android-owned object exists,
	 * replay the original instructions exactly.  On Switch x20 is null: prompt
 * for the email through libnx swkbd, then return through the native function's
 * existing epilogue without entering the unavailable-provider path. */
	cbnz	x20, .Laccount_email_bridge_nonnull

	/* Preserve all caller-saved integer/vector state and status flags around
	 * the blocking Switch keyboard applet.  Callee-saved x19-x29 (including
	 * the null x20) are preserved by the AArch64 ABI. */
	sub	sp, sp, #320
	stp	x0, x1, [sp]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]
	stp	q0, q1, [sp, #160]
	stp	q2, q3, [sp, #192]
	stp	q4, q5, [sp, #224]
	stp	q6, q7, [sp, #256]
	mrs	x9, nzcv
	mrs	x10, fpcr
	mrs	x11, fpsr
	str	x9, [sp, #288]
	str	x10, [sp, #296]
	str	x11, [sp, #304]

	bl	account_email_prompt

	ldr	x9, [sp, #288]
	ldr	x10, [sp, #296]
	ldr	x11, [sp, #304]
	msr	nzcv, x9
	msr	fpcr, x10
	msr	fpsr, x11
	ldp	x0, x1, [sp]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x30, [sp, #144]
	ldp	q0, q1, [sp, #160]
	ldp	q2, q3, [sp, #192]
	ldp	q4, q5, [sp, #224]
	ldp	q6, q7, [sp, #256]
	add	sp, sp, #320

	/* Do not resume +0x23F7720: that path dereferences the unavailable
	 * Android provider and is the source of libPVZ2+0x24005ec. The original
	 * epilogue at +0x23F779C restores the frame and returns normally. */
	adrp	x17, account_email_bridge_null_return
	ldr	x17, [x17, #:lo12:account_email_bridge_null_return]
	br	x17

.Laccount_email_bridge_nonnull:
	/* Exact original +0x23F7700..+0x23F770C. */
	ldr	x8, [x20]
	ldr	x8, [x8, #0x28]
	mov	x1, sp
	sub	x3, x29, #0x40
	adrp	x17, account_email_bridge_nonnull_continue
	ldr	x17, [x17, #:lo12:account_email_bridge_nonnull_continue]
	br	x17
	.size account_email_bridge_probe, .-account_email_bridge_probe

	/* diagnostic: direct stock archive-path diagnostics. These probes replay the
	 * exact four overwritten instructions after the observer returns, so the
	 * native gunzip/TAR code remains authoritative. */
	.align 2

	.align 2

	.align 2

	.align 2

	.align 2

	.align 2

	.align 2

	/*  exact OnlineIdentityService::tryValidation virtual getter
	 * call-site probes. hook_arm64 replaces four instructions at each BLR.
	 * Execute the original getter once, capture only the returned libc++
	 * string length while armed, replay the other three instructions exactly,
	 * restore the stock BLR return-address value in x30, and rejoin. */
	.align 2

	.align 2


	/* Startup deep-profiler trampolines.  Each function is called from a C
	 * timing wrapper.  The wrapper's LR is saved by the replayed native
	 * prologue, so the untouched native epilogue returns to the wrapper. */
	.align 2
	.global startup_state3_trampoline
	.type startup_state3_trampoline, %function
startup_state3_trampoline:
	/* libPVZ2 +0x13e86c4 .. +0x13e86d0 */
	sub	sp, sp, #0x60
	stp	x29, x30, [sp, #0x20]
	str	x23, [sp, #0x30]
	stp	x22, x21, [sp, #0x40]
	adrp	x17, startup_state3_continue
	ldr	x17, [x17, #:lo12:startup_state3_continue]
	br	x17
	.size startup_state3_trampoline, .-startup_state3_trampoline

	.align 2
	.global startup_content_refresh_trampoline
	.type startup_content_refresh_trampoline, %function
startup_content_refresh_trampoline:
	/* libPVZ2 +0x100c254 .. +0x100c260 */
	sub	sp, sp, #0x70
	stp	x29, x30, [sp, #0x40]
	stp	x22, x21, [sp, #0x50]
	stp	x20, x19, [sp, #0x60]
	adrp	x17, startup_content_refresh_continue
	ldr	x17, [x17, #:lo12:startup_content_refresh_continue]
	br	x17
	.size startup_content_refresh_trampoline, .-startup_content_refresh_trampoline

	.align 2
	.global startup_listener_iter_trampoline
	.type startup_listener_iter_trampoline, %function
startup_listener_iter_trampoline:
	/* libPVZ2 +0x173f500 .. +0x173f50c */
	stp	x29, x30, [sp, #-0x20]!
	stp	x20, x19, [sp, #0x10]
	mov	x29, sp
	ldp	x19, x20, [x0]
	adrp	x17, startup_listener_iter_continue
	ldr	x17, [x17, #:lo12:startup_listener_iter_continue]
	br	x17
	.size startup_listener_iter_trampoline, .-startup_listener_iter_trampoline

	.align 2
	.global startup_rm_child_a_trampoline
	.type startup_rm_child_a_trampoline, %function
startup_rm_child_a_trampoline:
	/* libPVZ2 +0x23b3c0c .. +0x23b3c18 */
	sub	sp, sp, #0x90
	stp	x29, x30, [sp, #0x50]
	stp	x24, x23, [sp, #0x60]
	stp	x22, x21, [sp, #0x70]
	adrp	x17, startup_rm_child_a_continue
	ldr	x17, [x17, #:lo12:startup_rm_child_a_continue]
	br	x17
	.size startup_rm_child_a_trampoline, .-startup_rm_child_a_trampoline

	.align 2
	.global startup_rm_child_b_trampoline
	.type startup_rm_child_b_trampoline, %function
startup_rm_child_b_trampoline:
	/* libPVZ2 +0x23b3eb8 .. +0x23b3ec4 */
	stp	x29, x30, [sp, #-0x40]!
	str	x23, [sp, #0x10]
	stp	x22, x21, [sp, #0x20]
	stp	x20, x19, [sp, #0x30]
	adrp	x17, startup_rm_child_b_continue
	ldr	x17, [x17, #:lo12:startup_rm_child_b_continue]
	br	x17
	.size startup_rm_child_b_trampoline, .-startup_rm_child_b_trampoline

	.align 2
	.global startup_rm_child_c_trampoline
	.type startup_rm_child_c_trampoline, %function
startup_rm_child_c_trampoline:
	/* libPVZ2 +0x23b3fb4 .. +0x23b3fc0 */
	sub	sp, sp, #0x1b0
	stp	x29, x30, [sp, #0x150]
	stp	x28, x27, [sp, #0x160]
	stp	x26, x25, [sp, #0x170]
	adrp	x17, startup_rm_child_c_continue
	ldr	x17, [x17, #:lo12:startup_rm_child_c_continue]
	br	x17
	.size startup_rm_child_c_trampoline, .-startup_rm_child_c_trampoline
