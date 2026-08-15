	.text
	.align 2
	.global register_touch_gameplay_object_trampoline
	.type register_touch_gameplay_object_trampoline, %function
register_touch_gameplay_object_trampoline:
	sub	sp, sp, #0xc0
	stp	x29, x30, [sp, #128]
	stp	x24, x23, [sp, #144]
	stp	x22, x21, [sp, #160]
	adrp	x17, register_touch_continue
	ldr	x17, [x17, #:lo12:register_touch_continue]
	br	x17
	.size register_touch_gameplay_object_trampoline, .-register_touch_gameplay_object_trampoline

	.align 2
	.global unregister_touch_gameplay_object_trampoline
	.type unregister_touch_gameplay_object_trampoline, %function
unregister_touch_gameplay_object_trampoline:
	stp	x29, x30, [sp, #-32]!
	stp	x20, x19, [sp, #16]
	mov	x29, sp
	mov	x20, x1
	adrp	x17, unregister_touch_continue
	ldr	x17, [x17, #:lo12:unregister_touch_continue]
	br	x17
	.size unregister_touch_gameplay_object_trampoline, .-unregister_touch_gameplay_object_trampoline

	.align 2
	.global game_input_on_touch_event_trampoline
	.type game_input_on_touch_event_trampoline, %function
game_input_on_touch_event_trampoline:
	sub	sp, sp, #0x90
	str	d8, [sp, #64]
	stp	x29, x30, [sp, #80]
	stp	x24, x23, [sp, #96]
	adrp	x17, game_input_on_touch_continue
	ldr	x17, [x17, #:lo12:game_input_on_touch_continue]
	br	x17
	.size game_input_on_touch_event_trampoline, .-game_input_on_touch_event_trampoline

	.align 2
.global game_input_dispatch_touch_trampoline
	.type game_input_dispatch_touch_trampoline, %function
game_input_dispatch_touch_trampoline:
	sub	sp, sp, #0xa0
	str	d10, [sp, #48]
	stp	d9, d8, [sp, #64]
	stp	x29, x30, [sp, #80]
	adrp	x17, game_input_dispatch_continue
	ldr	x17, [x17, #:lo12:game_input_dispatch_continue]
	br	x17
.size game_input_dispatch_touch_trampoline, .-game_input_dispatch_touch_trampoline

	/* Vtable-entry probes.  Each wrapper preserves the complete volatile
	 * register/vector state, asks C for the original target for this
	 * vtable+slot, restores the call exactly, then tail-branches to it. */
	.macro TOUCH_INTERFACE_SLOT n
	.align 2
	.global touch_interface_probe_\n
	.type touch_interface_probe_\n, %function
touch_interface_probe_\n:
	sub sp, sp, #512
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mov w0, #\n
	add x1, sp, #0
	ldr x2, [sp, #152]
	bl touch_interface_callback_probe
	str x0, [sp, #288]
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	ldr x17, [sp, #288]
	add sp, sp, #320
	br x17
	.size touch_interface_probe_\n, .-touch_interface_probe_\n
	.endm

	TOUCH_INTERFACE_SLOT 3

	.align 2
	.global touch_path_slot3_probe
	.type touch_path_slot3_probe, %function
touch_path_slot3_probe:
	/* Hooked over blr x9 at libPVZ2 +0x10dfc68.  Keep the pre-call
	 * register/SIMD state, log it, execute the original virtual call, keep
	 * the post-call state while logging the return, then replay the three
	 * instructions after the call and continue at +0x10dfc78. */
	sub sp, sp, #640
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mov x0, sp
	add x1, sp, #640
	ldr x2, [sp, #152]
	bl touch_path_slot3_before
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	blr x9
	stp x0, x1, [sp, #320]
	stp x2, x3, [sp, #336]
	stp x4, x5, [sp, #352]
	stp x6, x7, [sp, #368]
	stp x8, x9, [sp, #384]
	stp x10, x11, [sp, #400]
	stp x12, x13, [sp, #416]
	stp x14, x15, [sp, #432]
	stp x16, x17, [sp, #448]
	stp x18, x30, [sp, #464]
	stp q0, q1, [sp, #480]
	stp q2, q3, [sp, #512]
	stp q4, q5, [sp, #544]
	stp q6, q7, [sp, #576]
	mov x0, sp
	add x1, sp, #320
	ldr x2, [sp, #320]
	ldr x3, [sp, #152]
	bl touch_path_slot3_after
	ldp x0, x1, [sp, #320]
	ldp x2, x3, [sp, #336]
	ldp x4, x5, [sp, #352]
	ldp x6, x7, [sp, #368]
	ldp x8, x9, [sp, #384]
	ldp x10, x11, [sp, #400]
	ldp x12, x13, [sp, #416]
	ldp x14, x15, [sp, #432]
	ldp x16, x17, [sp, #448]
	ldp x18, x30, [sp, #464]
	ldp q0, q1, [sp, #480]
	ldp q2, q3, [sp, #512]
	ldp q4, q5, [sp, #544]
	ldp q6, q7, [sp, #576]
	add sp, sp, #640
	mov w8, w0
	ldr x0, [x19]
	add x9, x0, x23
	adrp x17, touch_path_slot3_continue
	ldr x17, [x17, #:lo12:touch_path_slot3_continue]
	br x17
	.size touch_path_slot3_probe, .-touch_path_slot3_probe

	.align 2
	.global touch_path_indirect_probe
	.type touch_path_indirect_probe, %function
touch_path_indirect_probe:
	/* Hooked over blr x8 at libPVZ2 +0x10dfd1c. */
	sub sp, sp, #640
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mov x0, sp
	add x1, sp, #640
	ldr x2, [sp, #152]
	bl touch_path_indirect_before
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	blr x8
	stp x0, x1, [sp, #320]
	stp x2, x3, [sp, #336]
	stp x4, x5, [sp, #352]
	stp x6, x7, [sp, #368]
	stp x8, x9, [sp, #384]
	stp x10, x11, [sp, #400]
	stp x12, x13, [sp, #416]
	stp x14, x15, [sp, #432]
	stp x16, x17, [sp, #448]
	stp x18, x30, [sp, #464]
	stp q0, q1, [sp, #480]
	stp q2, q3, [sp, #512]
	stp q4, q5, [sp, #544]
	stp q6, q7, [sp, #576]
	mov x0, sp
	add x0, x0, #320
	ldr x1, [sp, #320]
	ldr x2, [sp, #152]
	bl touch_path_indirect_after
	ldp x0, x1, [sp, #320]
	ldp x2, x3, [sp, #336]
	ldp x4, x5, [sp, #352]
	ldp x6, x7, [sp, #368]
	ldp x8, x9, [sp, #384]
	ldp x10, x11, [sp, #400]
	ldp x12, x13, [sp, #416]
	ldp x14, x15, [sp, #432]
	ldp x16, x17, [sp, #448]
	ldp x18, x30, [sp, #464]
	ldp q0, q1, [sp, #480]
	ldp q2, q3, [sp, #512]
	ldp q4, q5, [sp, #544]
	ldp q6, q7, [sp, #576]
	tbz w0, #0, 1f
	ldr w8, [sp, #688]
	cmp w8, #4
	b.eq 1f
	add sp, sp, #640
	adrp x17, touch_path_indirect_continue
	ldr x17, [x17, #:lo12:touch_path_indirect_continue]
	br x17
1:
	add sp, sp, #640
	adrp x17, touch_path_indirect_zero
	ldr x17, [x17, #:lo12:touch_path_indirect_zero]
	br x17
	.size touch_path_indirect_probe, .-touch_path_indirect_probe

	.align 2
	.global touch_registration_dispatch_probe
	.type touch_registration_dispatch_probe, %function
touch_registration_dispatch_probe:
	/* Hooked over the first four instructions of libPVZ2 +0x16f9730.
	 * This target is a tail dispatcher: after logging, reproduce its record
	 * lookup and branch directly to the selected callback. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mov x0, sp
	ldr x1, [sp, #152]
	bl touch_registration_dispatch_before
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	ldp x9, x8, [x0, #16]
	ldr x2, [x0, #8]
	add x0, x8, x9, asr #1
	tbz w9, #0, 1f
	ldr x8, [x0]
	ldr x2, [x8, x2]
1:
	add sp, sp, #320
	br x2
	.size touch_registration_dispatch_probe, .-touch_registration_dispatch_probe

	.align 2
	.global touch_registration_filter_probe
	.type touch_registration_filter_probe, %function
touch_registration_filter_probe:
	/* Hooked over the first four prologue instructions of +0x16f6518. */
	sub sp, sp, #768
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x2, nzcv
	str x2, [sp, #608]
	ldr x1, [sp, #152]
	mov x0, sp
	bl touch_registration_filter_before
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	ldr x2, [sp, #608]
	msr nzcv, x2
	adr x30, 1f
	str d8, [sp, #-48]!
	stp x29, x30, [sp, #8]
	str x21, [sp, #24]
	stp x20, x19, [sp, #32]
	adrp x17, touch_registration_filter_continue
	ldr x17, [x17, #:lo12:touch_registration_filter_continue]
	br x17
	1:
	/* The original function returned through this label.  Save its result
	 * state while the after-call logger runs, then restore it exactly. */
	stp x0, x1, [sp, #320]
	stp x2, x3, [sp, #336]
	stp x4, x5, [sp, #352]
	stp x6, x7, [sp, #368]
	stp x8, x9, [sp, #384]
	stp x10, x11, [sp, #400]
	stp x12, x13, [sp, #416]
	stp x14, x15, [sp, #432]
	stp x16, x17, [sp, #448]
	stp x18, x30, [sp, #464]
	stp q0, q1, [sp, #480]
	stp q2, q3, [sp, #512]
	stp q4, q5, [sp, #544]
	stp q6, q7, [sp, #576]
	mrs x2, nzcv
	str x2, [sp, #608]
	mov x0, sp
	add x0, x0, #320
	ldr x1, [sp, #320]
	ldr x2, [sp, #608]
	ldr x3, [sp, #152]
	bl touch_registration_filter_after
	ldp x0, x1, [sp, #320]
	ldp x2, x3, [sp, #336]
	ldp x4, x5, [sp, #352]
	ldp x6, x7, [sp, #368]
	ldp x8, x9, [sp, #384]
	ldp x10, x11, [sp, #400]
	ldp x12, x13, [sp, #416]
	ldp x14, x15, [sp, #432]
	ldp x16, x17, [sp, #448]
	ldp x18, x30, [sp, #464]
	ldp q0, q1, [sp, #480]
	ldp q2, q3, [sp, #512]
	ldp q4, q5, [sp, #544]
	ldp q6, q7, [sp, #576]
	ldr x30, [sp, #152]
	add sp, sp, #768
	ret
	.size touch_registration_filter_probe, .-touch_registration_filter_probe

	.align 2
	.global touch_registration_filter_branch_probe
	.type touch_registration_filter_branch_probe, %function
touch_registration_filter_branch_probe:
	/* Hooked over tbz w0,#0 at libPVZ2 +0x10dfd20. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mov x0, sp
	mov x1, x19
	mov x2, x20
	mov x3, x21
	mov x4, x22
	mov x5, x23
	add x6, sp, #320
	bl touch_registration_filter_branch
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	tbz w0, #0, 1f
	add sp, sp, #320
	adrp x17, touch_registration_filter_branch_continue
	ldr x17, [x17, #:lo12:touch_registration_filter_branch_continue]
	br x17
1:
	add sp, sp, #320
	adrp x17, touch_registration_filter_branch_zero
	ldr x17, [x17, #:lo12:touch_registration_filter_branch_zero]
	br x17
	.size touch_registration_filter_branch_probe, .-touch_registration_filter_branch_probe


	.align 2
	.global tutorial_state_trace_tutorial_state_probe
	.type tutorial_state_trace_tutorial_state_probe, %function
tutorial_state_trace_tutorial_state_probe:
	/* Replaces +0x11EB8C0..+0x11EB8CC:
	 *   sub sp,sp,#0x90
	 *   stp x29,x30,[sp,#0x30]
	 *   stp x28,x27,[sp,#0x40]
	 *   stp x26,x25,[sp,#0x50]
	 * Observe x0 TutorialLevel1, w1 requested internal state, and original LR;
	 * restore everything, replay the prologue exactly, resume +0x11EB8D0. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mov x2, x30
	bl tutorial_state_trace_tutorial_state_observed
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320
	sub sp, sp, #0x90
	stp x29, x30, [sp, #0x30]
	stp x28, x27, [sp, #0x40]
	stp x26, x25, [sp, #0x50]
	adrp x17, tutorial_state_trace_tutorial_state_continue
	ldr x17, [x17, #:lo12:tutorial_state_trace_tutorial_state_continue]
	br x17

	.size tutorial_state_trace_tutorial_state_probe, .-tutorial_state_trace_tutorial_state_probe

	.align 2
	.global state1_action_trace_state1_module_probe
	.type state1_action_trace_state1_module_probe, %function
state1_action_trace_state1_module_probe:
	/* Replaces +0x11ECC88..+0x11ECC94 in TutorialLevel1 state-1 entry:
	 *   mov w1,#5
	 *   mov w2,wzr
	 *   bl  +0x20C31B4
	 *   add x1,sp,#0x44
	 * Observe the already-resolved x0 object before/after the original helper.
	 * x19 is the TutorialLevel1 instance and x20 the LevelModuleManager in this
	 * native function. No state or object field is changed by the probe. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x9, nzcv
	mrs x10, fpcr
	mrs x11, fpsr
	str x9, [sp, #288]
	str x10, [sp, #296]
	str x11, [sp, #304]
	/* observer(module=x0, tutorial=x19, manager=x20, stage=before, result=0) */
	mov x1, x19
	mov x2, x20
	mov w3, #0
	mov x4, xzr
	bl state1_action_trace_state1_module_observed
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	ldr x11, [sp, #304]
	msr nzcv, x9
	msr fpcr, x10
	msr fpsr, x11
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320

	/* Keep original object across the native helper for post-observation. */
	sub sp, sp, #16
	str x0, [sp]
	mov w1, #5
	mov w2, wzr
	adrp x17, state1_action_trace_state1_helper_target
	ldr x17, [x17, #:lo12:state1_action_trace_state1_helper_target]
	blr x17

	/* Preserve exact helper return state while logging it. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x9, nzcv
	mrs x10, fpcr
	mrs x11, fpsr
	str x9, [sp, #288]
	str x10, [sp, #296]
	str x11, [sp, #304]
	ldr x4, [sp]          /* native helper result */
	ldr x0, [sp, #320]    /* original resolved module object */
	mov x1, x19
	mov x2, x20
	mov w3, #1
	bl state1_action_trace_state1_module_observed
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	ldr x11, [sp, #304]
	msr nzcv, x9
	msr fpcr, x10
	msr fpsr, x11
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320
	add sp, sp, #16

	/* Replay +0x11ECC94 and resume +0x11ECC98. */
	add x1, sp, #0x44
	adrp x17, state1_action_trace_state1_continue
	ldr x17, [x17, #:lo12:state1_action_trace_state1_continue]
	br x17

	.size state1_action_trace_state1_module_probe, .-state1_action_trace_state1_module_probe

	.align 2
	.global tutorial_entry_trace_tutorial_shared_entry_probe
	.type tutorial_entry_trace_tutorial_shared_entry_probe, %function
tutorial_entry_trace_tutorial_shared_entry_probe:
	/* Safe probe at the actual +0x11EC078 function entry. The earlier hook
	 * incorrectly hooked +0x11EBD7C..+0x11EBD88, but +0x11EBD84 is a
	 * shared epilogue target used by state 1. This entry hook cannot cover
	 * that target. Observe x0 (TutorialLevel1), restore all caller state,
	 * replay the exact four overwritten prologue instructions, and resume
	 * at +0x11EC088. No tutorial/input/module field is changed. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x9, nzcv
	mrs x10, fpcr
	mrs x11, fpsr
	str x9, [sp, #288]
	str x10, [sp, #296]
	str x11, [sp, #304]
	mov w1, #0
	bl tutorial_layout_trace_tutorial_shared_observed
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	ldr x11, [sp, #304]
	msr nzcv, x9
	msr fpcr, x10
	msr fpsr, x11
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320

	/* Exact native +0x11EC078..+0x11EC084 prologue. */
	sub sp, sp, #0x70
	str d8, [sp, #0x30]
	stp x29, x30, [sp, #0x40]
	stp x22, x21, [sp, #0x50]
	adrp x17, tutorial_entry_trace_tutorial_shared_continue
	ldr x17, [x17, #:lo12:tutorial_entry_trace_tutorial_shared_continue]
	br x17
	.size tutorial_entry_trace_tutorial_shared_entry_probe, .-tutorial_entry_trace_tutorial_shared_entry_probe


	.align 2
	.global state2_action_trace_state2_action_a_probe
	.type state2_action_trace_state2_action_a_probe, %function
state2_action_trace_state2_action_a_probe:
	/* Variant A. Replay the exact state-2 action argument setup that
	 * the selected native case used before +0x216DF40:
	 *   fmov s0,s8; add x2,sp,#8; mov x0,x20; mov x1,x21
	 * Observe those already-native arguments, restore every caller-visible
	 * register/FP status, then resume at the untouched original BL. */
	fmov s0, s8
	add x2, sp, #0x8
	mov x0, x20
	mov x1, x21
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x9, nzcv
	mrs x10, fpcr
	mrs x11, fpsr
	str x9, [sp, #288]
	str x10, [sp, #296]
	str x11, [sp, #304]
	bl state2_action_trace_state2_action_observed
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	ldr x11, [sp, #304]
	msr nzcv, x9
	msr fpcr, x10
	msr fpsr, x11
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320
	adrp x17, state2_action_trace_state2_action_continue
	ldr x17, [x17, #:lo12:state2_action_trace_state2_action_continue]
	br x17
	.size state2_action_trace_state2_action_a_probe, .-state2_action_trace_state2_action_a_probe

	.align 2
	.global state2_action_trace_state2_action_b_probe
	.type state2_action_trace_state2_action_b_probe, %function
state2_action_trace_state2_action_b_probe:
	/* Variant B differs only in the native object registers:
	 *   fmov s0,s8; add x2,sp,#8; mov x0,x19; mov x1,x20. */
	fmov s0, s8
	add x2, sp, #0x8
	mov x0, x19
	mov x1, x20
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x9, nzcv
	mrs x10, fpcr
	mrs x11, fpsr
	str x9, [sp, #288]
	str x10, [sp, #296]
	str x11, [sp, #304]
	bl state2_action_trace_state2_action_observed
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	ldr x11, [sp, #304]
	msr nzcv, x9
	msr fpcr, x10
	msr fpsr, x11
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320
	adrp x17, state2_action_trace_state2_action_continue
	ldr x17, [x17, #:lo12:state2_action_trace_state2_action_continue]
	br x17
	.size state2_action_trace_state2_action_b_probe, .-state2_action_trace_state2_action_b_probe

	.align 2
	.global state2_source_trace_state2_dispatch_source_probe
	.type state2_source_trace_state2_dispatch_source_probe, %function
state2_source_trace_state2_dispatch_source_probe:
	/* Exact native +0x11EC1C4 first instruction. Preserve the newly-loaded
	 * s8 across the C observer, then reproduce the jump-table address math
	 * and perform the original +0x11EC1D4 br x10 directly. */
	ldr s8, [x21, #0x10]
	sub sp, sp, #336
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x10, nzcv
	mrs x11, fpcr
	mrs x12, fpsr
	str x10, [sp, #288]
	str x11, [sp, #296]
	str x12, [sp, #304]
	str q8, [sp, #320]
	mov x0, x21
	fmov s0, s8
	bl state2_source_trace_state2_source_observed
	ldr x10, [sp, #288]
	ldr x11, [sp, #296]
	ldr x12, [sp, #304]
	msr nzcv, x10
	msr fpcr, x11
	msr fpsr, x12
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	ldr q8, [sp, #320]
	add sp, sp, #336

	/* Replay +0x11EC1C8..+0x11EC1D4. ADR is represented by the exact
	 * runtime target stored by main.c because the trampoline lives in the
	 * NRO, not libPVZ2. */
	adrp x10, state2_source_trace_state2_dispatch_base
	ldr x10, [x10, #:lo12:state2_source_trace_state2_dispatch_base]
	ldrh w11, [x9, x8, lsl #1]
	add x10, x10, x11, lsl #2
	br x10
	.size state2_source_trace_state2_dispatch_source_probe, .-state2_source_trace_state2_dispatch_source_probe

	.align 2
	.global tutorial_layout_trace_state1_layout0_probe
	.type tutorial_layout_trace_state1_layout0_probe, %function
tutorial_layout_trace_state1_layout0_probe:
	/* Runs after the original selector-0 +0x14383B4 call. Native state1 SP
	 * holds returned ints at +0x40/+0x44. Observe them, then replay:
	 * ldp w1,w0,[sp,#0x40]; ldr s0,[x21,#0x84]; mov w2,wzr; mov w3,wzr. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x9, nzcv
	mrs x10, fpcr
	mrs x11, fpsr
	str x9, [sp, #288]
	str x10, [sp, #296]
	str x11, [sp, #304]
	mov x0, x19
	mov w1, #0
	ldr w2, [sp, #(320 + 0x40)]
	ldr w3, [sp, #(320 + 0x44)]
	bl tutorial_layout_trace_state1_layout_observed
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	ldr x11, [sp, #304]
	msr nzcv, x9
	msr fpcr, x10
	msr fpsr, x11
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320
	ldp w1, w0, [sp, #0x40]
	ldr s0, [x21, #0x84]
	mov w2, wzr
	mov w3, wzr
	adrp x17, tutorial_layout_trace_state1_layout0_continue
	ldr x17, [x17, #:lo12:tutorial_layout_trace_state1_layout0_continue]
	br x17
	.size tutorial_layout_trace_state1_layout0_probe, .-tutorial_layout_trace_state1_layout0_probe

	.align 2
	.global tutorial_layout_trace_state1_layout3_probe
	.type tutorial_layout_trace_state1_layout3_probe, %function
tutorial_layout_trace_state1_layout3_probe:
	/* Runs after the original selector-3 +0x14383B4 call. Native state1 SP
	 * holds returned ints at +0x18/+0x1c. */
	sub sp, sp, #320
	stp x0, x1, [sp]
	stp x2, x3, [sp, #16]
	stp x4, x5, [sp, #32]
	stp x6, x7, [sp, #48]
	stp x8, x9, [sp, #64]
	stp x10, x11, [sp, #80]
	stp x12, x13, [sp, #96]
	stp x14, x15, [sp, #112]
	stp x16, x17, [sp, #128]
	stp x18, x30, [sp, #144]
	stp q0, q1, [sp, #160]
	stp q2, q3, [sp, #192]
	stp q4, q5, [sp, #224]
	stp q6, q7, [sp, #256]
	mrs x9, nzcv
	mrs x10, fpcr
	mrs x11, fpsr
	str x9, [sp, #288]
	str x10, [sp, #296]
	str x11, [sp, #304]
	mov x0, x19
	mov w1, #3
	ldr w2, [sp, #(320 + 0x18)]
	ldr w3, [sp, #(320 + 0x1c)]
	bl tutorial_layout_trace_state1_layout_observed
	ldr x9, [sp, #288]
	ldr x10, [sp, #296]
	ldr x11, [sp, #304]
	msr nzcv, x9
	msr fpcr, x10
	msr fpsr, x11
	ldp x0, x1, [sp]
	ldp x2, x3, [sp, #16]
	ldp x4, x5, [sp, #32]
	ldp x6, x7, [sp, #48]
	ldp x8, x9, [sp, #64]
	ldp x10, x11, [sp, #80]
	ldp x12, x13, [sp, #96]
	ldp x14, x15, [sp, #112]
	ldp x16, x17, [sp, #128]
	ldp x18, x30, [sp, #144]
	ldp q0, q1, [sp, #160]
	ldp q2, q3, [sp, #192]
	ldp q4, q5, [sp, #224]
	ldp q6, q7, [sp, #256]
	add sp, sp, #320
	ldp w1, w0, [sp, #0x18]
	ldr s0, [x21, #0x88]
	mov w2, wzr
	mov w3, wzr
	adrp x17, tutorial_layout_trace_state1_layout3_continue
	ldr x17, [x17, #:lo12:tutorial_layout_trace_state1_layout3_continue]
	br x17
	.size tutorial_layout_trace_state1_layout3_probe, .-tutorial_layout_trace_state1_layout3_probe

	.data
	.align 3
	.global register_touch_continue
register_touch_continue:
	.quad 0

	.global unregister_touch_continue
unregister_touch_continue:
	.quad 0

	.global game_input_on_touch_continue
game_input_on_touch_continue:
	.quad 0

	.global game_input_dispatch_continue
game_input_dispatch_continue:
	.quad 0

	.global touch_path_slot3_continue
touch_path_slot3_continue:
	.quad 0

	.global touch_registration_filter_continue
touch_registration_filter_continue:
	.quad 0

	.global touch_registration_filter_branch_continue
touch_registration_filter_branch_continue:
	.quad 0

	.global touch_registration_filter_branch_zero
touch_registration_filter_branch_zero:
	.quad 0

	.global touch_path_indirect_continue
touch_path_indirect_continue:
	.quad 0

	.global touch_path_indirect_zero
touch_path_indirect_zero:
	.quad 0

	.global tutorial_state_trace_tutorial_state_continue
tutorial_state_trace_tutorial_state_continue:
	.quad 0


	.global state1_action_trace_state1_helper_target
state1_action_trace_state1_helper_target:
	.quad 0

	.global state1_action_trace_state1_continue
state1_action_trace_state1_continue:
	.quad 0


	.global tutorial_entry_trace_tutorial_shared_continue
tutorial_entry_trace_tutorial_shared_continue:
	.quad 0

	.global state2_action_trace_state2_action_continue
state2_action_trace_state2_action_continue:
	.quad 0

	.global state2_source_trace_state2_dispatch_base
state2_source_trace_state2_dispatch_base:
	.quad 0

	.global tutorial_layout_trace_state1_layout0_continue
tutorial_layout_trace_state1_layout0_continue:
	.quad 0

	.global tutorial_layout_trace_state1_layout3_continue
tutorial_layout_trace_state1_layout3_continue:
	.quad 0
