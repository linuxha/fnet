;*-------------------------------------------------------------------
;* fnutil.inc - Code shared by the FlexNet utilities
;*
;*    Include it after a utility's own code. It defines:
;*      - the schar, rchar and resetv stubs, which netfind points at
;*        the vectors of the resident FNETDRV package;
;*      - netfind, the signature scan (6809-analyze.md section 5.1);
;*      - argcnt, sndarg, ndslot and pdata, the command line, drive
;*        map and message helpers that most utilities repeat.
;*
;*    Every utility also gets these variables, set by netfind:
;*      sgnadr  address of the 'netUUdrv' signature
;*      fnprsnt 1 if the 'FLEXNet 4.1.0' marker follows (FNETDRV),
;*              then drvptr -> drvltr and ndarray -> netdrv[0]
;*-------------------------------------------------------------------
;
;* Offsets in the driver block, from the signature (section 3)
;
SG_BEG	equ  21		;* package start = signature - SG_BEG
SG_SCH	equ  8		;* JMP schar
SG_RCH	equ  11		;* JMP rchar
SG_RST	equ  14		;* JMP reset
SG_MRK	equ  17		;* 'FLEXNet 4.1.0'
SG_DLT	equ  30		;* drvltr
SG_NDR	equ  34		;* netdrv[0..3]
SG_SIZ	equ  40		;* size of the package
;
;* Serial vectors, set by netfind. WARMS is a dummy target.
;
schar	jmp  WARMS	;* send A; C = 1 if sent. Preserves A and X.
rchar	jmp  WARMS	;* receive A; C = 1 if received. Preserves X.
resetv	jmp  WARMS	;* reset the ACIA
;
;*-------------------------------------------------------------------
;* netfind - Find the resident FNETDRV package and set the vectors.
;*    Tries MEMEND+1+SG_BEG first, where the installer puts it, then
;*    scans MEMEND+1 .. SCNEND-1.
;*    Exit: C = 1 if found, C = 0 if FlexNet is not loaded.
;*-------------------------------------------------------------------
netfind	ldx  MEMEND	;* X = MEMEND + 1 + SG_BEG
	ldab #1+SG_BEG
nfi02	inx
	decb
	bne  nfi02
	stx  nfw
	ldaa nfw	;* still below SCNEND?
	cmpa #SCNEND>>8
	bcc  nfi03	;* no
	bsr  nfsig	;* signature here?
	bcs  nfi08	;* yes
;
nfi03	ldx  MEMEND	;* scan from MEMEND+1
nfi04	inx
	cpx  #SCNEND	;* finished?
	beq  nfi06	;* yes, not found
	ldaa 0,x	;* quick test of the first byte
	cmpa nfsgn
	bne  nfi04
	bsr  nfsig
	bcc  nfi04
	bra  nfi08
;
nfi06	clc		;* not loaded
	rts
;
nfi08	stx  sgnadr	;* found
	ldab #SG_SCH	;* point the stubs at the vectors
	bsr  nfoff
	stx  schar+1
	ldab #SG_RCH
	bsr  nfoff
	stx  rchar+1
	ldab #SG_RST
	bsr  nfoff
	stx  resetv+1
	ldab #SG_DLT
	bsr  nfoff
	stx  drvptr
	ldab #SG_NDR
	bsr  nfoff
	stx  ndarray
;
	clr  fnprsnt	;* FNETDRV marker present?
	ldx  #nfmrk
	stx  nfp2
	ldab #SG_MRK
	bsr  nfoff
	ldab #nfmlen
	bsr  nfcmp
	bcc  nfi10	;* no
	inc  fnprsnt
nfi10	sec
	rts
;
;* nfoff - X = sgnadr + B
;
nfoff	clra
	addb sgnadr+1
	adca sgnadr
	staa nfw
	stab nfw+1
	ldx  nfw
	rts
;
;* nfsig - C = 1 if the signature is at X. Preserves X.
;
nfsig	ldab #nfslen
	stx  nfp1	;* keep X
	ldx  #nfsgn
	stx  nfp2
	ldx  nfp1
;
;* nfcmp - C = 1 if the B bytes at X equal those at (nfp2).
;*    Preserves X.
;
nfcmp	stx  nfcand
	stx  nfp1
nfc02	ldx  nfp1	;* byte from memory
	ldaa 0,x
	inx
	stx  nfp1
	ldx  nfp2	;* same as the string?
	cmpa 0,x
	bne  nfc06	;* no
	inx
	stx  nfp2
	decb
	bne  nfc02
	ldx  nfcand	;* equal
	sec
	rts
nfc06	ldx  nfcand	;* different
	clc
	rts
;
;*-------------------------------------------------------------------
;* argcnt - Measure the command line argument: skip spaces from
;*    TTYLBP, then count the characters up to the CR. TTYLBP is not
;*    moved.
;*    Exit: B = count (Z = 1 if none), A = first character.
;*-------------------------------------------------------------------
argcnt	ldx  TTYLBP
arg02	ldaa 0,x	;* skip leading spaces
	inx
	cmpa #' '
	beq  arg02
	staa nfw	;* first character
	clrb
arg04	cmpa #CR	;* end of line?
	beq  arg06
	incb		;* no, count it
	ldaa 0,x
	inx
	bra  arg04
arg06	ldaa nfw
	tstb
	rts
;
;*-------------------------------------------------------------------
;* sndarg - Send the rest of the command line to the host, from the
;*    first non-space character up to and including the CR. An EOL
;*    character ends it too, and is sent as CR; clear TTYEOL first
;*    to pass \ : ; through to the host.
;*    Exit: C = 0 on time-out.
;*-------------------------------------------------------------------
sndarg	jsr  NXTCH	;* skip leading spaces
	cmpa #' '
	beq  sndarg
snd02	cmpa TTYEOL	;* end of this command?
	bne  snd03
	ldaa #CR	;* yes, send CR instead
snd03	jsr  schar	;* send one character
	bcc  snd06	;* time-out
	cmpa #CR	;* last one?
	beq  snd04
	jsr  NXTCH
	bra  snd02
snd04	sec
snd06	rts
;
;*-------------------------------------------------------------------
;* ndslot - X = address of netdrv[A], A = drive 0-3. Only valid if
;*    fnprsnt is set. Destroys A.
;*-------------------------------------------------------------------
ndslot	ldx  ndarray
	tsta
	beq  nds04
nds02	inx
	deca
	bne  nds02
nds04	rts
;
;*-------------------------------------------------------------------
;* pdata - Print the NUL-terminated string at X (no CR/LF first)
;*-------------------------------------------------------------------
pdata	ldaa 0,x
	beq  pda02
	jsr  PUTCHR	;* preserves X
	inx
	bra  pdata
pda02	rts
;
;* Strings and variables
;
nfsgn	fcc  'netUUdrv'	;* package signature
nfslen	equ  *-nfsgn
nfmrk	fcc  'FLEXNet 4.1.0' ;* FNETDRV marker
nfmlen	equ  *-nfmrk
;
sgnadr	rmb  2		;* address of the signature
drvptr	rmb  2		;* -> drvltr in the package
ndarray	rmb  2		;* -> netdrv[0] in the package
fnprsnt	rmb  1		;* 1 if the package is FNETDRV
nfcand	rmb  2		;* scan candidate
nfp1	rmb  2		;* compare pointers
nfp2	rmb  2
nfw	rmb  2		;* temporary word
