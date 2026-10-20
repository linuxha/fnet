;*-------------------------------------------------------------------
;*
;* FEXIT - End the FlexNet session and remove FNETDRV
;*
;*    Port of src/6809/fexit.asm (1.01, js). It:
;*      - sends E, so the host closes the images and ends the session;
;*      - restores the 7 FLEX driver entries at DRVTBL;
;*      - zero-fills the package, so its signature is gone;
;*      - raises MEMEND, but only if the package sits directly above
;*        it (nothing was loaded below it since).
;*
;*    Usage: FEXIT
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 FEXIT 1.01. The "MEMEND
;*                          restored" message no longer runs into
;*                          "Program Exit" (defect D9).
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  1,1	;* version number
;
base	rmb  2		;* start of the package
size	rmb  2		;* size of the package
srcp	rmb  2		;* source pointer
dstp	rmb  2		;* destination pointer
tmp	rmb  1		;* saved interrupt mask
;
cold	jsr  netfind	;* find FNETDRV
	bcs  cold2
	jmp  noload
cold2	equ  *
;
;* base = signature - SG_BEG; size from the driver block
;
	ldaa sgnadr
	ldab sgnadr+1
	subb #SG_BEG
	sbca #0
	staa base
	stab base+1
	ldab #SG_SIZ
	jsr  nfoff	;* X -> size
	ldaa 0,x
	ldab 1,x
	staa size
	stab size+1
;
;* Send the Exit command and wait for ACK
;
	ldaa #'E'
	jsr  schar
	bcs  wtack
	jmp  nowork
wtack	jsr  rchar
	bcc  nowork
	cmpa #ACK
	bne  wtack
;
;* Restore the FLEX driver entries, with interrupts disabled so the
;*    spooler never jumps through a half-written address
;
	tpa
	staa tmp
	sei
	ldx  base
	stx  srcp
	ldx  #DRVTBL
	stx  dstp
	ldab #21	;* 7 entries
relp	ldx  srcp
	ldaa 0,x
	inx
	stx  srcp
	ldx  dstp
	staa 0,x
	inx
	stx  dstp
	decb
	bne  relp
	ldaa tmp	;* restore the interrupt mask
	tap
;
;* Zero-fill the package
;
	ldx  size	;* nothing to clear?
	beq  memchk
	stx  srcp	;* count
	ldx  base
fill	clr  0,x
	inx
	stx  dstp
	ldx  srcp	;* count down
	dex
	stx  srcp
	beq  memchk	;* done
	ldx  dstp
	bra  fill
;
memchk	ldaa MEMEND	;* MEMEND + 1 == base ?
	ldab MEMEND+1
	addb #1
	adca #0
	cmpa base
	bne  exitok	;* no, leave MEMEND alone
	cmpb base+1
	bne  exitok
	addb size+1	;* MEMEND = base + size
	adca size
	staa MEMEND
	stab MEMEND+1
	ldx  #restor	;* "MEMEND restored"
	bra  finish
;
noload	ldx  #nodrv	;* FNETDRV is not loaded
	bra  finish
;
exitok	ldx  #exitst	;* "Program Exit"
	bra  finish
;
nowork	ldx  #timout	;* time-out
finish	jsr  PSTRNG
	jmp  WARMS	;* back to FLEX
;
;* Messages
;
timout	fcc  "Communication time-out error"
	fcb  EOT
nodrv	fcc  "NETDRV is not loaded in memory, no action taken."
	fcb  EOT
restor	fcc  "MEMEND restored"
	fcb  EOT
exitst	fcc  "Program Exit"
	fcb  EOT
;
	include "fnutil.inc"
;
	if   * > UTLEND
	error "FEXIT does not fit in the utility command space"
	endif
	end  start
