;*-------------------------------------------------------------------
;*
;* RESYNC - Re-synchronize the serial link with the FlexNet host
;*
;*    Port of src/6809/resync.asm (2.02, BjB/js). Resets the ACIA
;*    through FNETDRV, then repeats the installer's sync: send $55
;*    until it is echoed, then $AA.
;*
;*    Usage: RESYNC
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 RESYNC 2.02
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  2,2	;* version number
;
tries	rmb  1		;* sync tries left
tmp	rmb  1		;* current sync character
;
cold	jsr  netfind	;* find FNETDRV
	bcc  noload
	jsr  resetv	;* reset the ACIA
	jsr  PCRLF
;
	ldaa #5		;* number of tries
	staa tries
	ldaa #$55	;* first sync character
sync	staa tmp
sync04	jsr  schar	;* send character
	bcc  sync16	;* time-out
	jsr  rchar	;* get the echo
	bcc  sync16
	cmpa tmp	;* same as sent?
	beq  sync08	;* yes
	ldaa tmp
	cmpa #$55	;* first sync character?
	bne  sync20	;* no, report error
	dec  tries
	bne  sync04	;* try again
	bra  sync20	;* report error
;
sync08	cmpa #$AA	;* second sync character?
	beq  sync12	;* yes, in sync
	ldaa #$AA	;* send the second sync character
	bra  sync
;
sync12	ldx  #succst	;* "Connection successfully ..."
	bra  sync24
;
sync16	ldx  #timest	;* "Time-out ..."
	bra  sync24
;
noload	ldx  #nodrv	;* FNETDRV is not loaded
	bra  sync24
;
sync20	ldx  #synest	;* "Could not synchronize ..."
sync24	jsr  PSTRNG
	jmp  WARMS	;* back to FLEX
;
;* Messages
;
succst	fcc  "Connection successfully established"
	fcb  EOT
timest	fcc  "Time-out error, connection broken!"
	fcb  BS,EOT
synest	fcc  "Could not synchronize the communication!"
	fcb  BS,EOT
nodrv	fcc  "NETDRV is not loaded in memory, no action taken."
	fcb  EOT
;
	include "fnutil.inc"
;
	if   * > UTLEND
	error "RESYNC does not fit in the utility command space"
	endif
	end  start
