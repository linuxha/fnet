;*-------------------------------------------------------------------
;*
;* FCREATE - Create a .DSK image on the FlexNet host
;*
;*    Port of src/6809/fcreate.asm (2.02, js). FNETDRV must be
;*    loaded. Prompts for the path, name, volume number, tracks and
;*    sectors per track, then asks before creating. With FNETDRV the
;*    new image is also mounted on drive d (default 3).
;*
;*    Usage: FCREATE [d]
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-20   Port from the 6809 FCREATE 2.02, with fixes:
;*                          - fnprsnt is tested, not memory $0000 (D3);
;*                          - FNETDRV is detected (D4);
;*                          - the drive is kept, and drive 0 no longer
;*                            ends the request (D5);
;*                          - numbers accept only 0-9 (D6), must not
;*                            be empty, and a corrected entry counts.
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"

	org  UTLORG
start	bra  cold
versn	fcb  2,2	;* version number
;
SEP	equ  CR		;* field separator in the request
BUFFER	equ  SYSFCB	;* request buffer
BUFEND	equ  BUFFER+250	;* end of the typed text; room for the CRs
;
ptr	rmb  2		;* pointer in the buffer
fldbeg	rmb  2		;* start of the field being typed
savptr	rmb  2		;* start of a number field, to retry it
promp	rmb  2		;* prompt of a number field
numflg	rmb  1		;* 0 if the field is a number
drv	rmb  1		;* drive to mount the image on
chr	rmb  1		;* typed character
tries	rmb  1		;* response time-outs left
;
cold	jsr  netfind	;* find FNETDRV
	bcs  cre01
	jmp  noload
cre01	ldx  #BUFFER
	stx  ptr
;
;* C, or c and a drive with FNETDRV
;
	ldaa #'C'
	tst  fnprsnt
	beq  cre02
	ldaa #'c'
cre02	jsr  putit
	tst  fnprsnt
	beq  cre08
;
cre04	jsr  NXTCH	;* skip leading spaces
	cmpa #' '
	beq  cre04
	ldab #3		;* default drive
	cmpa #CR	;* no drive given?
	beq  cre06
	cmpa TTYEOL
	beq  cre06
	suba #'0'	;* 0-3?
	cmpa #3
	bls  cre05
	jmp  baddrv
cre05	tab
cre06	stab drv
	tba
	jsr  putit	;* drive, binary
;
;* Prompt for the fields
;
cre08	ldx  #str1	;* path
	jsr  namin
	jsr  putsep
	ldx  #str2	;* name
	jsr  namin
	jsr  putsep
	ldx  #str3	;* volume number
	jsr  getdec
	jsr  putsep
	ldx  #str4	;* tracks
	jsr  getdec
	jsr  putsep
	ldx  #str5	;* sectors per track
	jsr  getdec
	jsr  putsep
;
	ldx  #go	;* ask for permission
	jsr  pdata
	jsr  GETCHR
	anda #$5F
	cmpa #'Y'
	bne  exit2
;
;* Send the request: every byte from BUFFER to ptr
;
	ldx  #BUFFER
cre10	ldaa 0,x
	jsr  schar
	bcc  nowork
	inx
	cpx  ptr
	bne  cre10
;
;* Wait for ACK or NAK. A large image takes a while to write, so
;*    allow 255 time-outs.
;
	ldaa #255
	staa tries
cre12	jsr  rchar
	bcs  cre14	;* got a character
	dec  tries
	bne  cre12
	bra  nowork
cre14	cmpa #ACK
	beq  good
	cmpa #NAK
	beq  bad
	bra  cre12
;
good	tst  fnprsnt	;* map the drive as remote
	beq  goo02
	ldaa drv
	jsr  ndslot
	ldaa drv
	staa 0,x
goo02	ldx  #goodst
	bra  exit
;
bad	ldx  #badst
	bra  exit
baddrv	ldx  #bdrvst
	bra  exit
noload	ldx  #nodrms	;* FNETDRV is not loaded
	bra  exit
nowork	ldx  #timout
exit	jsr  pdata
exit2	jmp  WARMS	;* back to FLEX
;
;*-------------------------------------------------------------------
;* namin - Print the prompt at X, then read a field into the buffer
;*    up to CR (not stored). Input is not echoed by FLEX; namin echoes
;*    it and handles backspace.
;*    Exit: numflg = 0 if the field is a non-empty decimal number.
;*-------------------------------------------------------------------
namin	jsr  pdata	;* prompt
	ldx  ptr
	stx  fldbeg
nam02	jsr  getne	;* get a character
	staa chr
	cmpa #BS	;* backspace?
	bne  nam06
	ldx  ptr	;* yes, anything to delete?
	cpx  fldbeg
	beq  nam02	;* no
	dex
	stx  ptr
	ldaa #BS	;* erase it on the screen
	jsr  PUTCHR
	ldaa #' '
	jsr  PUTCHR
	ldaa #BS
	jsr  PUTCHR
	bra  nam02
;
nam06	cmpa #CR	;* end of the field?
	beq  nam10
	ldx  ptr	;* buffer full?
	cpx  #BUFEND
	beq  nam02	;* yes, ignore it
	jsr  PUTCHR	;* echo it
	ldaa chr
	jsr  putit	;* and store it
	bra  nam02
;
nam10	jsr  PUTCHR	;* echo the CR
	clr  numflg	;* check for a number
	ldx  fldbeg
	cpx  ptr	;* empty?
	beq  nam14
nam12	ldaa 0,x
	cmpa #'0'
	bcs  nam14
	cmpa #'9'+1
	bcc  nam14
	inx
	cpx  ptr
	bne  nam12
	rts
nam14	inc  numflg	;* not a number
	rts
;
;*-------------------------------------------------------------------
;* getdec - Read a decimal number field (prompt at X), asking again
;*    until the field is a number
;*-------------------------------------------------------------------
getdec	stx  promp
	ldx  ptr	;* where the field starts
	stx  savptr
gde02	ldx  savptr	;* (again) from the start
	stx  ptr
	ldx  promp
	bsr  namin
	tst  numflg	;* a number?
	bne  gde02	;* no, ask again
	rts
;
;* getne - GETCHR without echo: clear the echo flag around the call
;
getne	ldab ECHOFL
	clr  ECHOFL
	jsr  GETCHR	;* preserves B
	stab ECHOFL
	rts
;
;* putsep - Put a separator in the buffer
;* putit  - Put A in the buffer
;
putsep	ldaa #SEP
putit	ldx  ptr
	staa 0,x
	inx
	stx  ptr
	rts
;
;* Messages
;
str1	fcb  CR,LF
	fcc  "File path ? "
	fcb  0
str2	fcb  CR,LF
	fcc  "File name ? "
	fcb  0
str3	fcb  CR,LF
	fcc  "Disk number (in decimal) ? "
	fcb  0
str4	fcb  CR,LF
	fcc  "Number of tracks (in decimal) ? "
	fcb  0
str5	fcb  CR,LF
	fcc  "Sectors per track (in decimal) ? "
	fcb  0
goodst	fcb  CR,LF
	fcc  "Command executed OK."
	fcb  0
badst	fcb  CR,LF
	fcc  "Error message received from NETPC."
	fcb  0
go	fcb  CR,LF
	fcc  "OK to proceed ? "
	fcb  0
timout	fcb  CR,LF
	fcc  "Communication time-out error"
	fcb  0
bdrvst	fcb  CR,LF
	fcc  "Bad drive number (the syntax is FCREATE [0-3])."
	fcb  0
nodrms	fcb  CR,LF
	fcc  "NETDRV is not loaded in memory, no action taken."
	fcb  0
;
	include "fnutil.inc"
;
	if   * > UTLEND
	error "FCREATE does not fit in the utility command space"
	endif
	end  start
