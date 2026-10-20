;*-------------------------------------------------------------------
;*
;* RCD - Change the current directory on the FlexNet host
;*
;*    Port of src/6809/rcd.asm (2.02, BjB/js). FNETDRV must be
;*    loaded.
;*
;*    Usage: RCD <path>
;*
;*    The FLEX EOL character is cleared while the path is sent, so
;*    the path may contain : and ;. RCD must therefore be the last
;*    command on a line.
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 RCD 2.02. A one-character
;*                          path (RCD /) is now accepted.
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  2,2	;* version number
;
eoltmp	rmb  1		;* saved EOL character
;
cold	jsr  netfind	;* find FNETDRV
	bcc  noload
	jsr  PCRLF
	ldaa TTYEOL	;* save the EOL character
	staa eoltmp
	jsr  argcnt	;* path given?
	beq  badfnm	;* no
;
	clr  TTYEOL	;* pass \ : ; through to the host
	ldaa #'P'	;* send P (Point) command
	jsr  schar
	bcc  nwrkng
	jsr  sndarg	;* then the path and CR
	bcc  nwrkng
	jsr  rchar	;* get response
	bcc  nwrkng
	cmpa #ACK
	bne  badfnm
	ldx  #succst	;* report success
	bra  finish
;
nwrkng	ldx  #nwrkst	;* communication is not working
	bra  finish
;
badfnm	ldx  #badfst	;* bad directory name
;
finish	ldaa eoltmp	;* restore the EOL character
	staa TTYEOL
	bra  fin02
;
noload	ldx  #nodrv	;* FNETDRV is not loaded
fin02	jsr  PSTRNG	;* print the message
	jmp  WARMS	;* back to FLEX
;
;* Messages
;
nwrkst	fcc  "Communication is not working!"
	fcb  BS,EOT
badfst	fcc  "Bad directory name!"
	fcb  BS,EOT
succst	fcc  "Command executed OK."
	fcb  EOT
nodrv	fcc  "NETDRV is not loaded in memory, no action taken."
	fcb  EOT
;
	include "fnutil.inc"
;
	if   * > UTLEND
	error "RCD does not fit in the utility command space"
	endif
	end  start
