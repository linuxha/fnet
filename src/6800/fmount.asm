;*-------------------------------------------------------------------
;*
;* FMOUNT - Mount a .DSK image from the FlexNet host as a FLEX drive
;*
;*    Port of src/6809/fmount.asm (2.04, BjB/js). FNETDRV must be
;*    loaded.
;*
;*    Usage: FMOUNT                 show which image each drive holds
;*           FMOUNT d               unmap drive d (on FLEX only)
;*           FMOUNT [d] name[.DSK]  mount name.DSK on drive d (0-3,
;*                                  default 3)
;*
;*    The extension, if given, must be .DSK; only the name is sent and
;*    the host adds the extension. Spaces in the name are dropped.
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 FMOUNT 2.04, with fixes:
;*                          - FMOUNT name mounts on drive 3 (D7);
;*                          - the drive is mapped only if the host
;*                            mounted the image;
;*                          - time-outs while showing the map are
;*                            reported.
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  2,4	;* version number
;
BUFFER	equ  SYSFCB	;* name / reply buffer
BUFEND	equ  BUFFER+255	;* last byte, kept for the terminator
;
eoltmp	rmb  1		;* saved EOL character
drv	rmb  1		;* drive number, 0-3
ptr	rmb  2		;* pointer in the buffer
extp	rmb  2		;* pointer in the extension string
msgp	rmb  2		;* message to print
;
cold	jsr  netfind	;* find FNETDRV
	bcs  cold2
	jmp  noload
cold2	jsr  PCRLF
	ldaa TTYEOL	;* save the EOL character
	staa eoltmp
;
	ldaa #'Q'	;* quick check that the host answers
	jsr  schar
	bcc  mnt01
	jsr  rchar
	bcc  mnt01
	cmpa #ACK
	beq  mnt015
mnt01	jmp  nwrkng
;
mnt015	clr  TTYEOL	;* pass \ : ; through
mnt02	jsr  NXTCH	;* skip leading spaces
	cmpa #' '
	beq  mnt02
	cmpa #CR	;* no argument?
	bne  mnt03
	jmp  shwmap	;* yes, show the map
;
;* A drive is a digit 0-3 on its own; anything else starts the name
;
mnt03	ldab #3		;* default drive
	cmpa #'0'
	bcs  mnt08
	cmpa #'4'
	bcc  mnt08
	ldx  TTYLBP	;* next character
	ldab 0,x
	cmpb #' '
	beq  mnt04
	cmpb #CR
	beq  mnt04
	ldab #3		;* a name that starts with a digit
	bra  mnt08
;
mnt04	suba #'0'	;* drive given
	staa drv
mnt06	jsr  NXTCH	;* skip spaces
	cmpa #' '
	beq  mnt06
	cmpa #CR	;* drive only?
	bne  mnt10
	jmp  unmap	;* yes, unmap it
;
mbadfn	jmp  badfnm	;* (in reach of the branches below)
mnwrk	jmp  nwrkng
;
mnt08	stab drv
;
;* Copy the name to the buffer up to the CR or the '.', dropping
;*    spaces, and end it with CR
;
mnt10	ldx  #BUFFER
	stx  ptr
mnt12	cmpa #CR	;* end of the name?
	beq  mnt20
	cmpa #'.'	;* extension?
	beq  mnt16
	cmpa #' '	;* drop spaces
	beq  mnt14
	ldx  ptr	;* store it
	cpx  #BUFEND
	beq  mbadfn	;* too long
	staa 0,x
	inx
	stx  ptr
mnt14	jsr  NXTCH
	bra  mnt12
;
mnt16	ldx  #ext	;* the extension must be DSK
	stx  extp
	ldab #3
mnt18	jsr  NXTCH
	anda #$5F	;* upper case
	ldx  extp
	cmpa 0,x
	bne  badex
	inx
	stx  extp
	decb
	bne  mnt18
;
mnt20	ldx  ptr	;* empty name?
	cpx  #BUFFER
	beq  mbadfn
	ldaa #CR	;* end it with CR
	staa 0,x
;
;* Send m, the drive (binary) and the name
;
	ldaa #'m'
	jsr  schar
	bcc  mnwrk
	ldaa drv
	jsr  schar
	bcc  mnwrk
	ldx  #BUFFER
mnt22	ldaa 0,x
	jsr  schar
	bcc  mnwrk
	inx
	cmpa #CR
	bne  mnt22
;
;* ACK, then R (read-only) or W (read/write); or NAK
;
	jsr  rchar
	bcc  mnwrk
	cmpa #ACK
	bne  mbadfn
	jsr  rchar
	bcc  mnwrk
	ldx  #readst
	cmpa #'R'
	beq  mnt24
	ldx  #writst
	cmpa #'W'
	bne  badfnm
mnt24	stx  msgp
	tst  fnprsnt	;* map the drive as remote
	beq  mnt26
	ldaa drv
	jsr  ndslot
	ldaa drv
	staa 0,x
mnt26	ldx  msgp
	bra  finish
;
;* FMOUNT d: unmap drive d on FLEX. The host keeps the image open.
;
unmap	tst  fnprsnt
	beq  nofnet
	ldaa drv
	jsr  ndslot
	ldaa #$FF
	staa 0,x
	ldx  #unmapp
	bra  finish
;
;* Messages and exits
;
badex	ldx  #exten	;* bad extension
	bra  finish
noload	ldx  #nodrv	;* FNETDRV is not loaded
	jsr  PSTRNG
	jmp  WARMS
nofnet	ldx  #nofnst	;* no drive map (not FNETDRV)
	bra  finish
nwrkng	ldx  #nwrkst	;* communication is not working
	bra  finish
badfnm	ldx  #badfst	;* could not open the file
finish	jsr  PSTRNG	;* print the message
alldone	ldaa eoltmp	;* restore the EOL character
	staa TTYEOL
	jmp  WARMS	;* back to FLEX
;
;*-------------------------------------------------------------------
;* shwmap - No argument: show the image on each remote drive
;*-------------------------------------------------------------------
shwmap	tst  fnprsnt
	beq  nofnet
	clr  drv
shw02	ldaa drv	;* drive mapped?
	jsr  ndslot
	ldaa 0,x
	cmpa #$FF
	beq  shw08	;* no
	ldaa #'d'	;* ask the host for its image name
	jsr  schar
	bcc  nwrkng
	ldaa drv
	jsr  schar
	bcc  nwrkng
	ldx  #BUFFER
	stx  ptr
shw04	jsr  rchar	;* collect the name up to ACK
	bcc  nwrkng
	cmpa #ACK
	beq  shw06
	ldx  ptr
	cpx  #BUFEND
	beq  shw04
	staa 0,x
	inx
	stx  ptr
	bra  shw04
;
shw06	ldx  ptr	;* print "Drive d -> name"
	clr  0,x
	ldaa drv
	adda #'0'
	staa mapdrv
	ldx  #mapped
	jsr  PSTRNG
	ldx  #BUFFER
	jsr  pdata
shw08	inc  drv	;* next drive
	ldaa drv
	cmpa #4
	bne  shw02
	jsr  PCRLF
	bra  alldone
;
;* Strings
;
ext	fcc  "DSK"
mapped	fcc  "Drive "
mapdrv	fcc  "  -> "
	fcb  EOT
exten	fcc  "Illegal file extension"
	fcb  EOT
nwrkst	fcc  "Communication is not working!"
	fcb  EOT
badfst	fcc  "Could not open file"
	fcb  EOT
readst	fcc  "File open in read-only mode"
	fcb  EOT
writst	fcc  "File opened with full access (read/write)"
	fcb  EOT
unmapp	fcc  "Drive unmapped"
	fcb  EOT
nofnst	fcc  "FNETDRV (FLEXNet 4.1.0) is not loaded, no drive map."
	fcb  EOT
nodrv	fcc  "NETDRV is not loaded in memory, no action taken."
	fcb  EOT
;
	include "fnutil.inc"
;
	if   * > UTLEND
	error "FMOUNT does not fit in the utility command space"
	endif
	end  start
