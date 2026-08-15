	.text
	.align 2
	.global wwise_init_probe
	.type wwise_init_probe, %function
wwise_init_probe:
	adrp	x17, wwise_init_result
	add	x17, x17, :lo12:wwise_init_result
	str	w0, [x17]
	cmp	w0, #1
	b.ne	1f

	/* Instructions replaced at libPVZ2.so +0x23f2d1c/+0x23f2d20. */
	mov	w8, #0x40000000
	str	w8, [sp, #32]
	adrp	x17, wwise_init_success
	ldr	x17, [x17, #:lo12:wwise_init_success]
	br	x17

1:
	adrp	x17, wwise_init_failure
	ldr	x17, [x17, #:lo12:wwise_init_failure]
	br	x17
	.size wwise_init_probe, .-wwise_init_probe

	.align 2
	.global wwise_term_trampoline
	.type wwise_term_trampoline, %function
wwise_term_trampoline:
	/* Instructions replaced at libPVZ2.so +0x25dafe4. */
	stp	x29, x30, [sp, #-96]!
	stp	x28, x27, [sp, #16]
	stp	x26, x25, [sp, #32]
	stp	x24, x23, [sp, #48]
	adrp	x17, wwise_term_continue
	ldr	x17, [x17, #:lo12:wwise_term_continue]
	br	x17
	.size wwise_term_trampoline, .-wwise_term_trampoline

	.align 2
	.global wwise_loadbank_probe
	.type wwise_loadbank_probe, %function
wwise_loadbank_probe:
	/* Replay the four instructions replaced at libPVZ2+0x25DC798. */
	sub	sp, sp, #0xb0
	stp	x29, x30, [sp, #0x80]
	stp	x22, x21, [sp, #0x90]
	stp	x20, x19, [sp, #0xa0]

	/* The native LoadBank path enters with x0=bank name and x1=bank-id output.
	 * Preserve both across the passive C logger, then resume the untouched body
	 * at +0x25DC7A8. */
	stp	x0, x1, [sp, #0x00]
	bl	wwise_loadbank_observe
	ldp	x0, x1, [sp, #0x00]
	adrp	x17, wwise_loadbank_continue
	ldr	x17, [x17, #:lo12:wwise_loadbank_continue]
	br	x17
	.size wwise_loadbank_probe, .-wwise_loadbank_probe

	.align 2
	.global audio_event_probe
	.type audio_event_probe, %function
audio_event_probe:
	/* Semantically reproduce libPVZ2+0x149F014 after a passive event-name log.
	 * Preserve all arguments that the backend slot receives, plus the caller LR. */
	sub	sp, sp, #0x20
	stp	x0, x1, [sp, #0x00]
	stp	x2, x30, [sp, #0x10]
	mov	x0, x1
	bl	audio_event_observe
	ldp	x0, x1, [sp, #0x00]
	ldp	x2, x30, [sp, #0x10]
	add	sp, sp, #0x20

	ldr	x0, [x0, #0x8]
	cbz	x0, 1f
	ldr	x8, [x0]
	ldr	x3, [x8, #0x38]
	br	x3
1:
	ret
	.size audio_event_probe, .-audio_event_probe
