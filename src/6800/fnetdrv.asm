;*-------------------------------------------------------------------
;*
;* FNETDRV - FlexNet remote .DSK driver for FLEX 6800 on an MC6800
;*
;*    Port of src/6809/fnetdrv.asm (FLEXNet 4.1.0, BjB/js). Mounts
;*    disk images on a FlexNet host (fnetd) over a 6850 serial line
;*    as FLEX drives 0-3. See src/6809/6809-analyze.md.
;*
;*    The file has two parts:
;*      - The resident package, linked at PKGORG. The installer
;*        copies it below MEMEND and relocates it.
;*      - The installer, at UTLORG. It runs once and is then
;*        overwritten.
;*
;*    Usage: FNETDRV [d [d ...]]   map drive(s) d (0-3) to the host
;*
;*    Rev      Date         Desc
;*    0.0.1    2026-10-19   Port from the 6809 FNETDRV 4.1.0
;*
;*-------------------------------------------------------------------
	cpu  6800
	include "flex68.inc"
	include "acia68.inc"

	ifndef PKGORG
PKGORG	equ  $1000	;* link address of the package (above page 0)
	endif
;
;*-------------------------------------------------------------------
;* rel - Assemble one instruction whose 2-byte operand is an
;*    address inside the package, and record the operand's offset
;*    for the installer's relocation table (reltbl).
;*
;*    Every absolute reference into the package MUST use this macro.
;*    "make -C src/6800 check" assembles the package at two origins
;*    and fails if any reference was missed.
;*-------------------------------------------------------------------
rcnt	set  0
rel	macro op,arg
rl{"\{rcnt}"} equ *+1-pkgbeg
	op   arg
rcnt	set  rcnt+1
	endm
;
;*-------------------------------------------------------------------
;* Resident package
;*
;*    The block from fread to size must stay together, byte for byte
;*    as on the 6809: the utilities find it by its signature and use
;*    fixed offsets from there (6809-analyze.md section 3).
;*-------------------------------------------------------------------
	org  PKGORG
pkgbeg
;
;* Original FLEX driver entries, copied from DRVTBL by the installer
;
fread	rmb  3		;* read single sector
fwrite	rmb  3		;* write single sector
fverfy	rmb  3		;* verify write operation
frestr	rmb  3		;* restore head to track 00
fdrive	rmb  3		;* drive selection
fcheck	rmb  3		;* check ready
fquick	rmb  3		;* quick check ready
;
sgnst	fcc  'netUUdrv'	;* signature
len	equ  *-sgnst
;
	rel  jmp,schar	;* vector for send character
	rel  jmp,rchar	;* vector for receive character
	rel  jmp,reset	;* vector for ACIA reset
;
	fcc  'FLEXNet 4.1.0'
;
drvltr	fcb  0,0,0,0	;* host drive letter ('@' = floppy, long time-out)
netdrv	fcb  $FF,$FF,$FF,$FF ;* netdrv[d] = d if drive d is remote, else $FF
;
qcheck	fcb  0		;* 1 = send Q before every sector
slowpc	fcb  0		;* 1 = delay after each command byte
;
size	fdb  pkgsiz	;* size of the package
;
;* End of the block
;
;* Local variables
;
curdrv	rmb  1		;* drive saved by RESTORE/DRIVE/CHKRDY/QUICK
curtrk	rmb  2		;* track and sector of the current request
chksum	rmb  2		;* sector checksum, then status of the last transfer
rcvsum	rmb  1		;* checksum MSB received from the host
cnt	rmb  1		;* byte counter
lstdrv	rmb  1		;* drive of the last write (for VERIFY)
delcnt	rmb  1		;* inner time-out counter
dlycnt	rmb  1		;* inner counter of the slow-PC delay
bufp	rmb  2		;* caller's X during a transfer
savecc	rmb  1		;* caller's CC (interrupt mask) during a transfer
savex	rmb  2		;* X saved by rchar/schar
lkx	rmb  2		;* X saved by isnet
;
;*-------------------------------------------------------------------
;* nread - READ entry: read sector B of track A into (X)
;*    Exit: B = status (0, 9 = checksum, 16 = not ready), Z = 1 if OK
;*-------------------------------------------------------------------
nread	rel  staa,curtrk	;* save track and sector
	rel  stab,curtrk+1
	rel  ldaa,curdrv
	bsr  isnet	;* remote drive?
	beq  nrea02	;* yes
	rel  ldaa,curtrk	;* no, restore A and B
	rel  ldab,curtrk+1
	rel  jmp,fread	;* and do the FLEX read
;
nrea02	rel  jsr,xfrini	;* start the transfer
	rel  jsr,qchk	;* Q check if enabled
	bcc  nrea10	;* not ready
	ldaa #'s'	;* send sector command
	rel  jsr,scmd
	bcc  nrea10
;
nrea04	rel  jsr,rchar	;* read one byte
	bcc  nrea10
	staa 0,x	;* store in the buffer
	inx
	rel  adda,chksum+1	;* update checksum
	rel  staa,chksum+1
	bcc  nrea08
	rel  inc,chksum
nrea08	rel  dec,cnt	;* 256 bytes
	bne  nrea04
;
	rel  jsr,rchar	;* get checksum MSB
	bcc  nrea10
	rel  staa,rcvsum
	rel  jsr,rchar	;* get checksum LSB
	bcc  nrea10
	rel  cmpa,chksum+1	;* compare checksums
	bne  nrea12
	rel  ldaa,rcvsum
	rel  cmpa,chksum
	bne  nrea12
;
	ldaa #ACK	;* good sector
	rel  jsr,schar
	bcc  nrea10
	clrb		;* report OK
	bra  nrea16
;
nrea10	ldab #16	;* report not ready
	bra  nrea16
;
nrea12	ldaa #NAK	;* bad checksum
	rel  jsr,schar
	bcc  nrea10
	ldab #9		;* report read error (CRC)
;
nrea16	rel  stab,chksum	;* status for VERIFY
	rel  jmp,ioend
;
;*-------------------------------------------------------------------
;* isnet - Z = 1 if drive A (0-3) is mapped to the host.
;*    Preserves A and X, destroys B.
;*-------------------------------------------------------------------
isnet	rel  stx,lkx
	rel  ldx,#netdrv
	tab
	andb #$03
	beq  isn04
isn02	inx		;* X -> netdrv[A]
	decb
	bne  isn02
isn04	ldab 0,x
	rel  ldx,lkx
	cba		;* netdrv[A] == A ?
	rts
;
;*-------------------------------------------------------------------
;* nwrite - WRITE entry: write sector B of track A from (X)
;*    Exit: B = status (0, 10 = write error, 16 = not ready), Z = 1 if OK
;*-------------------------------------------------------------------
nwrite	rel  staa,curtrk	;* save track and sector
	rel  stab,curtrk+1
	rel  ldaa,curdrv
	rel  staa,lstdrv	;* last drive written to
	bsr  isnet	;* remote drive?
	beq  nwri02	;* yes
	rel  ldaa,curtrk	;* no, restore A and B
	rel  ldab,curtrk+1
	rel  jmp,fwrite	;* and do the FLEX write
;
nwri02	rel  jsr,xfrini	;* start the transfer
	rel  jsr,qchk	;* Q check if enabled
	bcc  nwri10	;* not ready
	ldaa #'r'	;* receive sector command
	rel  jsr,scmd
	bcc  nwri10
	rel  jsr,slow
;
nwri04	ldaa 0,x	;* get byte from the buffer
	inx
	rel  jsr,schar
	bcc  nwri10
	rel  adda,chksum+1	;* update checksum
	rel  staa,chksum+1
	bcc  nwri08
	rel  inc,chksum
nwri08	rel  dec,cnt	;* 256 bytes
	bne  nwri04
;
	rel  ldaa,chksum	;* send checksum MSB
	rel  jsr,schar
	bcc  nwri10
	rel  ldaa,chksum+1	;* send checksum LSB
	rel  jsr,schar
	bcc  nwri10
;
	rel  jsr,rchar	;* get response
	bcc  nwri10
	cmpa #ACK
	bne  nwri12
	clrb		;* report OK
	bra  nwri16
;
nwri10	ldab #16	;* report not ready
	bra  nwri16
;
nwri12	ldab #10	;* report write error
;
nwri16	rel  stab,chksum	;* status for VERIFY
	bra  ioend
;
;*-------------------------------------------------------------------
;* xfrini - Start a sector transfer: clear the checksum and the byte
;*    counter, then fall into ioini.
;*
;* ioini - Start a host exchange. If called from the spooler task,
;*    yield first (as t/dskdrv.lst does). Save X and the caller's CC,
;*    then disable interrupts so no character is lost.
;*-------------------------------------------------------------------
xfrini	rel  clr,chksum
	rel  clr,chksum+1
	rel  clr,cnt	;* 256 bytes
ioini	tst  PRCNT	;* here from the spooler task?
	beq  ioi02	;* no
	swi		;* yes, suspend the spooler
	nop		;* allow time for interrupts
ioi02	rel  stx,bufp	;* save the caller's X
	tpa
	rel  staa,savecc	;* save the interrupt mask
	sei
	rts
;
;*-------------------------------------------------------------------
;* ioend - End a transfer with status B: restore X and the caller's
;*    interrupt mask, and set Z = 1 if B = 0.
;*-------------------------------------------------------------------
ioend	rel  ldx,bufp
	rel  ldaa,savecc
	tap
	tstb
	rts
;
;*-------------------------------------------------------------------
;* nverfy - VERIFY entry: return the status of the last remote write
;*-------------------------------------------------------------------
nverfy	rel  ldaa,lstdrv	;* was the last write remote?
	rel  jsr,isnet
	beq  nver02	;* yes
	rel  jmp,fverfy	;* no, do the FLEX verify
nver02	rel  ldab,chksum	;* status of the last write
	rts
;
;*-------------------------------------------------------------------
;* nrestr, ndrsel - RESTORE and DRIVE entries (X = FCB). Save the
;*    drive for READ/WRITE; nothing to do for a remote drive.
;*-------------------------------------------------------------------
nrestr	ldaa 3,x	;* get requested drive
	rel  staa,curdrv
	rel  jsr,isnet
	beq  nok	;* remote
	rel  jmp,frestr	;* local, do the FLEX restore
;
ndrsel	ldaa 3,x	;* get requested drive
	rel  staa,curdrv
	rel  jsr,isnet
	beq  nok	;* remote
	rel  jmp,fdrive	;* local, do the FLEX drive select
;
nok	clrb		;* nothing to do for a remote drive
	rts
;
;*-------------------------------------------------------------------
;* ncheck, nquick - CHKRDY and QUICK entries (X = FCB). For a remote
;*    drive, send Q and expect ACK.
;*    Exit: B = 0, Z = 1, C = 0 if ready; B = 16, Z = 0, C = 1 if not
;*-------------------------------------------------------------------
ncheck	ldaa 3,x	;* get requested drive
	rel  staa,curdrv
	rel  jsr,isnet
	beq  nqui04	;* remote
	rel  jmp,fcheck	;* local, do the FLEX check ready
;
nquick	ldaa 3,x	;* get requested drive
	rel  staa,curdrv
	rel  jsr,isnet
	beq  nqui04	;* remote
	rel  jmp,fquick	;* local, do the FLEX quick check
;
nqui04	bsr  ioini
	ldaa #'Q'	;* quick check command
	bsr  schar
	bcc  nqui08
	bsr  rchar	;* get response
	bcc  nqui08
	cmpa #ACK
	bne  nqui08	;* not ready
	clrb		;* report ready
	bra  ioend
;
nqui08	ldab #16	;* report not ready
	bsr  ioend
	sec
	rts
;
;*-------------------------------------------------------------------
;* rchar - Receive a character from the host.
;*    Exit: C = 1 and A = character, or C = 0 on time-out.
;*    Preserves X, destroys B.
;*-------------------------------------------------------------------
rchar	rel  stx,savex
	bsr  tmo	;* X = outer time-out count
	rel  clr,delcnt	;* inner count = 256
rcha04	ldab aciac	;* character received?
	asrb		;* RDRF -> C
	bcs  rcha08	;* yes
	rel  dec,delcnt
	bne  rcha04
	dex
	bne  rcha04
	rel  ldx,savex	;* time-out
	clc
	rts
;
rcha08	ldaa aciad	;* read the character
	rel  ldx,savex
	sec
	rts
;
;*-------------------------------------------------------------------
;* schar - Send character A to the host.
;*    Exit: C = 1 if sent, C = 0 on time-out.
;*    Preserves A and X, destroys B.
;*-------------------------------------------------------------------
schar	rel  stx,savex
	bsr  tmo	;* X = outer time-out count
	rel  clr,delcnt	;* inner count = 256
scha04	ldab aciac	;* transmit register empty?
	asrb
	asrb		;* TDRE -> C
	bcs  scha08	;* yes
	rel  dec,delcnt
	bne  scha04
	dex
	bne  scha04
	rel  ldx,savex	;* time-out
	clc
	rts
;
scha08	staa aciad	;* send the character
	rel  ldx,savex
	sec
	rts
;
;*-------------------------------------------------------------------
;* tmo - X = outer time-out count: 100 (about 0.5 s at 1 MHz), or
;*    65535 if the host is on a floppy (drvltr = '@'). Destroys B.
;*-------------------------------------------------------------------
tmo	ldx  #100	;* default
	rel  ldab,drvltr
	cmpb #'@'	;* floppy?
	bne  tmo02	;* no
	ldx  #65535	;* longer time-out
tmo02	rts
;
;*-------------------------------------------------------------------
;* qchk - If qcheck is set, send Q and expect ACK.
;*    Exit: C = 1 if ready (or not checked), C = 0 if not.
;*-------------------------------------------------------------------
qchk	rel  tst,qcheck
	beq  qch08	;* not enabled
	ldaa #'Q'	;* send Q command
	bsr  schar
	bcc  qch09
	bsr  rchar	;* get response
	bcc  qch09
	cmpa #ACK
	bne  qch06	;* not ready
	bsr  slow
qch08	sec
qch09	rts
qch06	clc
	rts
;
;*-------------------------------------------------------------------
;* scmd - Send command A, then the drive, track and sector.
;*    Exit: C = 0 on time-out.
;*-------------------------------------------------------------------
scmd	bsr  schar	;* command
	bcc  scm09
	bsr  slow
	rel  ldaa,curdrv	;* drive number
	bsr  schar
	bcc  scm09
	rel  ldaa,curtrk	;* track
	bsr  schar
	bcc  scm09
	rel  ldaa,curtrk+1	;* sector
	bsr  schar
scm09	rts
;
;*-------------------------------------------------------------------
;* slow - Delay after a command byte if slowpc is set. Destroys B.
;*-------------------------------------------------------------------
slow	rel  tst,slowpc
	beq  slo08
	ldab #50	;* change if needed
	rel  clr,dlycnt
slo04	rel  dec,dlycnt
	bne  slo04
	decb
	bne  slo04
slo08	rts
;
;*-------------------------------------------------------------------
;* reset - ACIA master reset, then 8 bits, 1 stop, clock / 16
;*-------------------------------------------------------------------
reset	ldaa #$03	;* master reset
	staa aciac
	ldaa #$15	;* 8N1, /16
	staa aciac
	rts
;
pkgend
pkgsiz	equ  pkgend-pkgbeg
;
;*-------------------------------------------------------------------
;* Installer
;*
;*    Runs once from the utility command space. It is overwritten
;*    by the next command.
;*-------------------------------------------------------------------
	org  UTLORG
;
start	bra  init
versn	fcb  4,1	;* version number
;
;* Package-relative entry points for the FLEX driver table
;
newtbl	fdb  nread-pkgbeg	;* read single sector
	fdb  nwrite-pkgbeg	;* write single sector
	fdb  nverfy-pkgbeg	;* verify write operation
	fdb  nrestr-pkgbeg	;* restore head to track 00
	fdb  ndrsel-pkgbeg	;* drive select
	fdb  ncheck-pkgbeg	;* check drive ready
	fdb  nquick-pkgbeg	;* quick check drive ready
;
;* Display the greeting and version number
;
init	ldaa versn	;* make the version ASCII
	adda #'0'
	staa v1
	ldaa versn+1
	adda #'0'
	staa v1+2
	ldx  #greet
	jsr  PSTRNG
;
;* Scan MEMEND+1 .. SCNEND-1 for the signature, in case FlexNet is
;*    already loaded
;
	ldx  MEMEND
sear2	inx		;* bump pointer
	cpx  #SCNEND	;* finished?
	beq  sear6	;* yes, not found
	ldaa 0,x	;* quick test of the first byte
	cmpa sgnst
	bne  sear2
	stx  cand	;* candidate address
	stx  srcp
	ldx  #sgnst
	stx  dstp
	ldab #len
sear3	ldx  srcp	;* byte from RAM
	ldaa 0,x
	inx
	stx  srcp
	ldx  dstp	;* same as the signature?
	cmpa 0,x
	bne  sear4	;* no
	inx
	stx  dstp
	decb		;* next byte
	bne  sear3
;
	ldx  #alread	;* found: already loaded
	jmp  exit
;
sear4	ldx  cand	;* restart after the candidate
	bra  sear2
;
;* Not loaded. Get the drive numbers from the command line.
;
sear6	jsr  GETHEX	;* get hex number
	bcs  nonum	;* not valid
	tstb
	beq  nonum	;* no number
	stx  tmpw
	ldaa tmpw+1	;* A = drive, limited to 0-3
	anda #$03
	ldx  #netdrv
	tab
	beq  sear8
sear7	inx		;* X -> netdrv[A]
	decb
	bne  sear7
sear8	staa 0,x	;* map the drive
	bra  sear6	;* allow multiple drives
;
;* Reset the ACIA, then sync: send $55 until it is echoed, then
;*    $AA. This verifies that 8 bits are transferred correctly.
;
nonum	jsr  reset
	ldaa #5		;* number of tries
	staa tries
	ldaa #$55	;* first sync character
sync04	staa tmp
sync08	jsr  schar	;* send character
	bcc  sync16	;* time-out
	jsr  rchar	;* get the echo
	bcc  sync16
	cmpa tmp	;* same as sent?
	beq  sync12	;* yes
	ldaa tmp
	cmpa #$55	;* first sync character?
	bne  sync16	;* no, something is wrong
	dec  tries
	bne  sync08	;* try again
	bra  sync16	;* sync error
;
sync12	cmpa #$AA	;* second sync character?
	beq  sync20	;* yes, in sync
	ldaa #$AA	;* send the second sync character
	bra  sync04
;
sync16	ldx  #synstr	;* "Can't sync ..."
	jmp  exit
;
sync20	ldx  #scnest	;* "Serial connection established"
	jsr  PSTRNG
;
;* "Where am I": send '?' and receive the host's current directory.
;*    The first character sets drvltr ('@' = floppy, long time-out);
;*    all are printed until the final ACK.
;
	ldaa #'?'
	jsr  schar
	bcc  sync16
	jsr  rchar	;* first character
	bcc  sync16
	staa tmp
	suba #1		;* A/B becomes @/A
	anda #$5E	;* make upper case
	staa drvltr
	ldx  #drvmsg	;* "Current host directory is "
	jsr  PSTRNG
	ldaa tmp
wtack	jsr  PUTCHR	;* print it
	jsr  rchar	;* until ACK
	bcc  sync16
	cmpa #ACK
	bne  wtack
;
;* Make room below MEMEND: base = MEMEND - pkgsiz, as on the 6809,
;*    so that FEXIT can restore MEMEND = base + size.
;
	ldaa MEMEND
	ldab MEMEND+1
	subb #pkgsiz&255
	sbca #pkgsiz>>8
	staa base
	stab base+1
	subb #pkgend&255	;* base must be above the loaded package
	sbca #pkgend>>8
	bcc  movtbl
	ldx  #nomem	;* "Not enough memory"
	jmp  exit
;
;* Copy the FLEX driver table into the package
;
movtbl	ldx  #DRVTBL
	stx  srcp
	ldx  #pkgbeg
	stx  dstp
	ldx  #21	;* 7 entries
	stx  cntw
	jsr  move
;
;* Copy the package to base, then relocate it
;
	ldaa #$55	;* dummy drive number, so that READ/WRITE
	staa curdrv	;*   and VERIFY do not select a remote drive
	staa lstdrv	;*   before FLEX's first DRIVE call
	ldx  #pkgbeg
	stx  srcp
	ldx  base
	stx  dstp
	ldx  #pkgsiz
	stx  cntw
	jsr  move
;
	ldaa base+1	;* delta = base - PKGORG
	suba #PKGORG&255
	staa delta+1
	ldaa base
	sbca #PKGORG>>8
	staa delta
;
	ldx  #reltbl
movrel	cpx  #relend	;* done?
	beq  movadr
	stx  srcp
	ldaa 0,x	;* A:B = base + offset of operand
	ldab 1,x
	addb base+1
	adca base
	staa dstp
	stab dstp+1
	ldx  dstp	;* operand += delta
	ldab 1,x
	addb delta+1
	stab 1,x
	ldaa 0,x
	adca delta
	staa 0,x
	ldx  srcp	;* next entry
	inx
	inx
	bra  movrel
;
;* Patch the FLEX driver table with interrupts disabled, so the
;*    spooler never jumps through a half-written address
;
movadr	tpa
	staa tmp
	sei
	ldx  #newtbl
	stx  srcp
	ldx  #DRVTBL+1	;* operand of the first JMP
	stx  dstp
	ldab #7		;* number of entries
mova02	ldx  srcp	;* tmpw = base + newtbl[n]
	ldaa 1,x
	adda base+1
	staa tmpw+1
	ldaa 0,x
	adca base
	staa tmpw
	inx
	inx
	stx  srcp
	ldx  dstp
	ldaa tmpw
	staa 0,x
	ldaa tmpw+1
	staa 1,x
	inx		;* next JMP
	inx
	inx
	stx  dstp
	decb
	bne  mova02
	ldaa tmp	;* restore the interrupt mask
	tap
;
	ldaa base+1	;* MEMEND = base - 1
	ldab base
	suba #1
	sbcb #0
	staa MEMEND+1
	stab MEMEND
;
;* Report the mapped drives
;
	ldx  #instst	;* "Remote .DSK drives installed:"
	jsr  PSTRNG
	ldx  base	;* X -> netdrv in the installed package
	ldab #netdrv-pkgbeg
inst02	inx
	decb
	bne  inst02
	ldaa #4		;* 4 drives
	staa tries
inst04	ldaa 0,x
	bmi  inst06	;* not mapped
	stx  tmpw
	adda #'0'
	staa tmp
	ldaa #' '
	jsr  PUTCHR
	ldaa tmp
	jsr  PUTCHR
	ldx  tmpw
inst06	inx
	dec  tries
	bne  inst04
	jsr  PCRLF
	jmp  WARMS
;
exit	jsr  PSTRNG	;* print message and return to FLEX
	jmp  WARMS
;
;*-------------------------------------------------------------------
;* move - Copy cntw bytes from (srcp) to (dstp). cntw must not be 0.
;*-------------------------------------------------------------------
move	ldx  srcp
	ldaa 0,x
	inx
	stx  srcp
	ldx  dstp
	staa 0,x
	inx
	stx  dstp
	ldx  cntw
	dex
	stx  cntw
	bne  move
	rts
;
;* Messages
;
greet	fcc  "FLEXNet driver version "
v1	fcb  0,'.',0,EOT
synstr	fcc  "Can't sync serial transfer!"
	fcb  EOT
scnest	fcc  "Serial connection established"
	fcb  EOT
drvmsg	fcc  "Current host directory is "
	fcb  EOT
instst	fcc  "Remote .DSK drives installed:"
	fcb  EOT
alread	fcc  "FLEXNet is already loaded, no action taken."
	fcb  EOT
nomem	fcc  "Not enough memory for FLEXNet."
	fcb  EOT
;
;* Relocation table: offset of each package operand that holds an
;*    address inside the package (built by the rel macro)
;
reltbl
i	set  0
	rept rcnt
	fdb  rl{"\{i}"}
i	set  i+1
	endm
relend
;
;* Installer variables
;
base	rmb  2		;* address of the installed package
delta	rmb  2		;* base - PKGORG
srcp	rmb  2		;* source pointer
dstp	rmb  2		;* destination pointer
cntw	rmb  2		;* byte count
cand	rmb  2		;* signature scan candidate
tmpw	rmb  2		;* temporary word
tmp	rmb  1		;* temporary byte
tries	rmb  1		;* sync tries left
;
	end  start
