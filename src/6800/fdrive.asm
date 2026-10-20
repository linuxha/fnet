;*-------------------------------------------------------------------
;*
;* FDRIVE - Change the host drive, or show the host's current
;*    directory
;*
;*    Port of src/6809/fdrive.asm (2.04, js). FNETDRV must be
;*    loaded. The first character of the drive (or of the current
;*    directory) sets FNETDRV's drvltr: A or B ('@'/'A' after the
;*    conversion) selects the long, floppy time-out. fnetd ACKs and
;*    ignores the drive.
;*
;*    Usage: FDRIVE [drive]    no argument: show the current directory
;*
;*    The FLEX EOL character is cleared while the drive is sent, so
;*    FDRIVE must be the last command on a line.
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 FDRIVE 2.04
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  2,4	;* version number
;
BUFFER	equ  SYSFCB	;* reply buffer
BUFEND	equ  BUFFER+255	;* last byte, kept for the terminator
;
eoltmp	rmb  1		;* saved EOL character
first	rmb  1		;* first character of the argument
ptr	rmb  2		;* pointer in the buffer
;
cold	jsr  netfind	;* find FNETDRV
	bcs  cold2
	jmp  noload
cold2	ldaa TTYEOL	;* save the EOL character
	staa eoltmp
	jsr  argcnt	;* argument given?
	beq  where	;* no, show the current directory
	staa first
;
	clr  TTYEOL	;* pass \ : ; through to the host
	ldaa #'V'	;* send V (driVe) command
	jsr  schar
	bcc  nwrkng
	jsr  sndarg	;* then the drive and CR
	bcc  nwrkng
	jsr  rchar	;* get response
	bcc  nwrkng
	cmpa #ACK
	bne  badfnm
	ldaa first	;* set drvltr
	bsr  setltr
	ldx  #succst	;* report success
	bra  finish
;
;* No argument: "where am I". Send ?, collect the reply up to the
;*    CR, set drvltr from its first character and print it.
;
where	ldaa #'?'
	jsr  schar
	bcc  nwrkng
	ldx  #BUFFER
	stx  ptr
whe02	jsr  rchar	;* get a character
	bcc  nwrkng
	cmpa #CR	;* end of the directory?
	beq  whe04
	ldx  ptr	;* store it, if there is room
	cpx  #BUFEND
	beq  whe02
	staa 0,x
	inx
	stx  ptr
	bra  whe02
;
whe04	ldx  ptr	;* terminate it
	clr  0,x
	ldaa BUFFER	;* set drvltr
	bsr  setltr
	ldx  #curdir	;* print it
	jsr  PSTRNG
	ldx  #BUFFER
	jsr  pdata
whe06	jsr  rchar	;* wait for ACK
	bcc  nwrkng
	cmpa #ACK
	bne  whe06
	jmp  WARMS
;
;* setltr - drvltr = (A - 1) & $5E: a/b become @/A, in upper case
;
setltr	tst  fnprsnt	;* FNETDRV?
	beq  set02	;* no, drvptr is not valid
	deca
	anda #$5E
	ldx  drvptr
	staa 0,x
set02	rts
;
nwrkng	ldx  #nwrkst	;* communication is not working
	bra  finish
;
badfnm	ldx  #badfst	;* bad name
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
curdir	fcc  "The current directory is  "
	fcb  EOT
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
	error "FDRIVE does not fit in the utility command space"
	endif
	end  start
