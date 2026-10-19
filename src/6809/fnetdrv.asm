*   File name   FLEXNet.txt
*
*   This is a driver package which implements
*   a remote mounted ".DSK file drive" over a serial line.
*
*** If the remote end is running on a slow machine, you might have
*   to remove the "***" comments, which will activate some delays
*   and "hand shaking". There is also a constant in the "delay"
*   routine, that can be changed for fine tuning. "As is," (with
*   the "***" comments removed) the constants have been tuned to
*   run with a 40 MHz 386 PC as the host computer.
*
*   Also, the "odelc" constant might have to be increased, if the
*   remote end has a slow HD. Otherwise, a time-out error might
*   occur during file pointer positioning.
*
*
*   vn  01.00   2000-09-15, BjB
*       01.01   2000-09-19, BjB:    fixing typos and omissions
*       02.00   2000-09-24, BjB:    time-out if comm link is broken
*       02.01   2000-09-24, BjB:    fixed verify
*       02.03   2000-09-30, BjB:    options for "fast/slow PC"
*       03.00   2002-05-01, js      Add jump vectors for rchar/schar
*       03.01   2002-08-30, js      Add string search routine to
*                                   avoid duplicate loads
*       03.02   2002-09-19, JS      Add ACIA reset vector and move all
*                                   vectors after signature
*       03.03   2002-11-23, js      Add the "remember drive letter" function
*                                   and longer delay for floppies
*       03.04   2002-11-29  js      Add a few pointers for uninstall
*
* ---------------------------------------------------------------
*
*   This part will be relocated below the FLEX [MEMEND].
*   The code must be position-independent so that it remains
*   functional after relocation!
*
* ---------------------------------------------------------------
*
        org     $0000


* The following lines should all stay together: jump table,
* signature, vectors, and a few pointers. The Rxxx.CMD utilities
* expect them to keep the same relative position, so if they
* are moved they MUST be moved together as one single block.
*
*   FLEX disk driver jump table
*
fread   rmb     3               read single sector
fwrite  rmb     3               write single sector
fverfy  rmb     3               verify write operation
frestr  rmb     3               restore head to track# 00
fdrive  rmb     3               drive selection
fcheck  rmb     3               check ready
fquick  rmb     3               quick check ready

* signature string

sgnst   fcc     'netUUdrv'
len     equ     *-sgnst

        lbra    schar           vector for send character
        lbra    rchar           vector for receive character
        lbra    reset           vector for ACIA reset

        fcc     'FLEXNet 4.1.0'

drvltr  rmb     4               MS-DOS drive letter
netdrv  fcb     -1              Flex drive selected as DOS drive
        fcb     -1              -1 means no drive mapped
        fcb     -1
        fcb     -1

qcheck  fcb     0               0 = do not do Quick Check before reading and writing sectors
*                               1 = do Quick Check before reading and writing sectors
slowpc  fcb     1               0 = not slow PC
*                               1 = Slow PC


size    fdb     drvend          Size of drivers

* End of "block"

*
*   Local variables
*
curdrv  rmb     1               current drive from fcb
curtrk  rmb     2               current ttss#
chksum  rmb     2               checksum
cnt     rmb     1               div counter
lstdrv  rmb     1               latest drive# selected
delcnt  rmb     1               inner time-out delay counter
*                               (default is drive 3)
odelc   rmb     2               max delay
*
*   Read one sector from 'net drive'
*
nread   pshs    a,x
        lda     -64+3,x         get requested drive#
        sta     curdrv,pcr
        leax    netdrv,pcr
        leax    a,x
        cmpa    0,x             same as assigned 'net drive'#?

*       cmpa    netdrv,pcr      same as assigned 'net drive'#?

        puls    x,a
        bne     fread           no, do FLEX read routine

        pshs    x               save FCB pointer
        std     curtrk,pcr      save current ttss#
        clr     chksum,pcr      clear checksum
        clr     chksum+1,pcr
        clr     cnt,pcr         256 bytes to read
*
*   Q(uick) Check that remote drive is ready
*
        lda     qcheck,pcr
        beq     nqchk1

        lda     #'Q             Send Q command
        lbsr    schar
        lbcc    nrea10          "Drive not ready" time out
        lbsr    rchar           get response
        lbcc    nrea10          time out
        cmpa    #ack            got an ack?
        lbne    nrea10          nope, report error

        lda     slowpc,pcr
        beq     nqchk1

        lbsr    delay           for "slow PC" ***

nqchk1  lda     #'s             Send sector command
        lbsr    schar
        bcc     nrea10          "Drive not ready"

        lda     slowpc,pcr
        beq     ntslw1

        lbsr    delay           for "slow PC" ***

ntslw1  lda     curdrv,pcr      drive number
        lbsr    schar
        bcc     nrea10
        lda     curtrk,pcr      tt#
        lbsr    schar
        bcc     nrea10
        lda     curtrk+1,pcr    ss#
        lbsr    schar
        bcc     nrea10

nrea04  lbsr    rchar           read one byte
        bcc     nrea10
        sta     ,x+             store in FCB and move pointer
        adda    chksum+1,pcr    update checksum lsb
        sta     chksum+1,pcr
        bcc     nrea08          bra if no carry
        inc     chksum,pcr      update checksum msb

nrea08  dec     cnt,pcr         decrease byte count
        bne     nrea04          loop till 0

        lbsr    rchar           get checksum msb
        bcc     nrea10
        pshs    a               save for now
        lbsr    rchar           get checksum lsb

        tfr     a,b             make lsb
        puls    a               restore msb
        bcc     nrea10          time out?

        cmpd    chksum,pcr      compare checksums
        bne     nrea12          bra if checksum err

        lda     #ack            send ack char
        lbsr    schar
        bcc     nrea10
        clrb                    report okay
        bra     nrea16

nrea10  ldb     #16             report Drive not ready
        bra     nrea16

nrea12  lda     #nak            send nak char
        lbsr    schar
        bcc     nrea10
        ldb     #09             report read error (CRC)

nrea16  stb     chksum,pcr      for later test
        tstb                    for FLEX error check
        puls    x               restore FCB pointer
        rts
*
*   Write one sector to 'net drive'
*
nwrite  pshs    a,x
        lda     -64+3,x         get requested drive#
        sta     curdrv,pcr
        sta     lstdrv,pcr      last drive written to
        leax    netdrv,pcr
        leax    a,x
        cmpa    0,x             same as assigned 'net drive'#?

*       cmpa    netdrv,pcr      same as assigned 'net drive'?

        puls    x,a
        lbne    fwrite          no, do FLEX write routine

        pshs    x               save FCB pointer
        std     curtrk,pcr      save current ttss#
        clr     chksum,pcr      clear checksum
        clr     chksum+1,pcr
        clr     cnt,pcr         256 bytes to send
*
*   Q(uick) Check that remote drive is ready
*
        lda     qcheck,pcr
        beq     nqchk2

        lda     #'Q             Send Q command
        lbsr    schar
        lbcc    nwri10          "Drive not ready" time out
        lbsr    rchar           get response
        lbcc    nwri10          time out
        cmpa    #ack            got an ack?
        lbne    nwri10          nope, report error

        lda     slowpc,pcr
        beq     nqchk2

        lbsr    delay           for "slow PC" ***

nqchk2  lda     #'r             Receive sector command
        lbsr    schar
        bcc     nwri10          "Drive not ready"

        lda     slowpc,pcr
        beq     ntslw2

        lbsr    delay           for "slow PC" ***

ntslw2  lda     curdrv,pcr      drive number
        lbsr    schar
        bcc     nwri10
        lda     curtrk,pcr      tt#
        lbsr    schar
        bcc     nwri10
        lda     curtrk+1,pcr    ss#
        lbsr    schar
        bcc     nwri10

        lda     slowpc,pcr
        beq     nwri04

        lbsr    delay           for "slow PC" ***

nwri04  lda     ,x+             get byte from FCB and move pointer
        lbsr    schar
        bcc     nwri10
        adda    chksum+1,pcr    update checksum lsb
        sta     chksum+1,pcr
        bcc     nwri08          bra if no carry
        inc     chksum,pcr      update checksum msb

nwri08  dec     cnt,pcr         decrease byte count
        bne     nwri04

        lda     chksum,pcr      send checksum msb
        lbsr    schar
        bcc     nwri10
        lda     chksum+1,pcr    send checksum lsb
        lbsr    schar
        bcc     nwri10

        lbsr    rchar           get response
        bcc     nwri10
        cmpa    #ack
        bne     nwri12          bra if not ack

        clrb                    report okay
        bra     nwri16

nwri10  ldb     #16             report Drive not ready
        bra     nwri16

nwri12  ldb     #10             disk file write error

nwri16  stb     chksum,pcr      for later check
        tstb                    for FLEX error check
        puls    x               restore FCB pointer
        rts
*
*   Verify last sector written
*
nverfy  lda     lstdrv,pcr      was last drive# = new drive#?
        pshs    x
        leax    netdrv,pcr
        leax    a,x
        cmpa    0,x             same as assigned 'net drive'#?

*       cmpa    netdrv,pcr

        puls    x
        lbne    fverfy          no, do FLEX verify routine

        ldb     chksum,pcr      get latest checksum test result
        tstb
        rts
*
*   Restore to track# 00
*
nrestr  lda     3,x             get requested drive#
        pshs    x
        leax    netdrv,pcr
        leax    a,x
        cmpa    0,x             same as assigned 'net drive'#?

*       cmpa    netdrv,pcr      same as assigned 'net drive'#?

        puls    x
        lbne    frestr          no, do FLEX restore routine

        clrb                    nothing to do with 'net drive'
        rts
*
*   Drive select
*
ndrsel  lda     3,x             get requested drive#
        pshs    x
        leax    netdrv,pcr
        leax    a,x
        cmpa    0,x             same as assigned 'net drive'#?

*       cmpa    netdrv,pcr      same as assigned 'net drive'#?

        puls    x
        lbne    fdrive          no, do FLEX drive select routine

        clrb                    nothing to do with 'netdrv'
        rts
*
*   Check drive ready
*
ncheck  lda     3,x             get requested drive#
        pshs    x
        leax    netdrv,pcr
        leax    a,x
        cmpa    0,x             same as assigned 'net drive'#?

*       cmpa    netdrv,pcr      same as assigned for 'net drive'#?

        puls    x
        lbne    fcheck          no, do FLEX check drive ready routine
        bra     nqui04          common for Check & Quick Check
*
*   Quick check drive ready
*
nquick  lda     3,x             get requested drive#
        pshs    x
        leax    netdrv,pcr
        leax    a,x
        cmpa    0,x             same as assigned 'net drive'#?

*       cmpa    netdrv,pcr      same as assigned for 'net drive'#?

        puls    x
        lbne    fquick          no, do FLEX Quick Check routine

nqui04  lda     #'Q             quick check command
        bsr     schar
        bcc     nqui08

        bsr     rchar           get response
        bcc     nqui08
        cmpa    #ack
        bne     nqui08          not ready

        clrb                    report drive ready
        bra     nqui12

nqui08  ldb     #16             report drive not ready
        sec

nqui12  tstb
        rts
*
*   Receive character.
*   Returns with character in ACCA and CC set if successful,
*                           CC cleared if time-out occurred.
*
rchar   pshs    y
        bsr     dlyset          go set delay
        ldy     odelc,pcr       outer delay counter
        clr     delcnt,pcr      inner delay counter

rcha04  ldb     aciac           check if char received
        asrb
        bcs     rcha08          get character

        dec     delcnt,pcr      decrement inner delay counter
        bne     rcha04          continue if not = 0
        leay    -1,y            decrement outer delay counter
        bne     rcha04          continue if not = 0
        bra     rcha12          return with CC cleared

rcha08  lda     aciad           read char
rcha12  puls    y,pc
*
*   Send character.
*   Returns with CC set if successful,
*                CC cleared if time-out occurred.
*
schar   pshs    y
        bsr     dlyset          go set proper delay
        ldy     odelc,pcr       outer delay counter
        clr     delcnt,pcr      inner delay counter

scha04  ldb     aciac           check if tdr is empty
        asrb
        asrb
        bcs     scha08          OK, send char

        dec     delcnt,pcr      decrement inner delay counter
        bne     scha04          continue if not = 0
        leay    -1,y            decrement outer delay counter
        bne     scha04          continue if not = 0
        bra     scha12          return with CC cleared

scha08  sta     aciad           send char
scha12  puls    y,pc
*
*   Delay routine (for "slow PC")
*
delay   clrb
        pshs    b
        ldb     #50             change if needed
dela04  dec     0,s
        bne     dela04
        decb
        bne     dela04
        puls    b,pc
*
*  ACIA reset routine
*
reset   lda     #$03            ACIA master reset
        sta     aciac
        lda     #$15            8 bits, 1 stop, clk/16
        sta     aciac
        rts
*
*  Delay set routine
*  Sets the content of "odelc" as a function of
*  the drive type; destroys y and b
*
dlyset  ldy     #100            default value
        ldab    drvltr,pcr      get drive letter
        cmpb    #$40            is it floppy?
        bne     dlexit          no, don't change
        ldy     #65535          select longer delay
dlexit  sty     odelc,pcr
        rts


drvend  equ     *               end of driver package
*
* ---------------------------------------------------------------
*
*   FLEX equates
*
warms   equ     $cd03           FLEX warm start
pstrng  equ     $cd1e           write string to display
pcrlf   equ     $cd24           write cr/lf to display
putchr  equ     $cd18           write character to display
gethex  equ     $cd42           get hex number
*
memend  equ     $cc2b           FLEX end of user RAM
drvtbl  equ     $de00           start of FLEX driver jump table
*
*   Misc equates
*
ack     equ     $06             acknowledge character
nak     equ     $15             negative acknowledge

tmp     equ     lstdrv          re-use for temp storage
tries   equ     cnt             re-use for number of tries
*
* ---------------------------------------------------------------
*
*   The following code will be dropped after a successful
*   line synchronization and relocation of the driver routines.
*
* ---------------------------------------------------------------
*
        org     $c100
*
*   New jump address table
*
newtbl  fdb     nread           read single sector
        fdb     nwrite          write single sector
        fdb     nverfy          verify write operation
        fdb     nrestr          restore head to track# 00
        fdb     ndrsel          drive select
        fdb     ncheck          check drive ready
        fdb     nquick          quick check drive ready

*---------------------------------------------------------
*
*   Start of installer program
*
*
*---------------------------------------------------------

start   bra     init
versn   fcb     4,1             version number

init    clr a                   reset DP register
        tfr     a,dp            (just in case...)

*
* Display greeting message and version number
*
        ldx     #greet          point to string
        ldd     versn           get version number
        addd    #$3030          make ASCII
        staa    v1
        stab    v1+2
        jsr     pstrng          go print string
*
* Scan memory from MEMEMD to $C000 to find
* out if a copy of FLEXNet is already loaded
*
search  ldx     memend          start of search
sear2   leax    1,x             Bump pointer
        cpx     #$c000          Finished?
        beq     sear4           Yes, not found
        ldy     #sgnst          Point to target string
        clrb                    Reset byte counter
sear3   ldaa    b,x             Get byte from RAM
        cmpa    b,y             Same as signature?
        bne     sear2           No, bump and restart
        incb                    Point to next byte
        cmpb    #len            Finished?
        bne     sear3           No, check next byte
*
* string found, already in memory; tell user
*
        ldx     #alread         already loaded...
        bra     sync17          display then exit
*
* Search done and no match found;
* initialize FLEXNet and go!
*
sear4   equ     *

* Get drive number from user

        jsr     gethex          get hex number
        bcs     nonum           skip if not valid
        tstb
        beq     nonum
        tfr     x,d             transfer number to d
        andb    #$03            limit to 3

        pshs    x
        leax    netdrv,pcr
        leax    b,x
        stab    0,x             store in target drive #

*       stab    netdrv          store in target drive #

        puls    x

        bra     sear4           allow multiple drives

nonum equ *
*
*   Initialize ACIA.
*
        LIB     ACIA
*
        jsr     reset           call the ACIA reset routine

* default to short delay (i.e. hard disk)
*
        ldx     #$4000
        stx     odelc

*   Check if host is ready; "sync" with $55
*   and then $aa. This will verify that 8 bits
*   are transferred correctly.
*
sync    lda     #5              number of tries
        sta     tries
        lda     #$55            1:st sync char
sync04  sta     tmp

sync08  lbsr    schar           send char
        bcc     sync16          time out, report error

        lbsr    rchar           get answer from receiver
        bcc     sync16
        cmpa    tmp             same as sent?
        beq     sync12          yes

        lda     tmp
        cmpa    #$55            1:st sync char?
        bne     sync16          nope, something is wrong

        dec     tries           decrease try count
        bne     sync08          try again if not 0
        bra     sync16          report sync error

sync12  cmpa    #$aa            2:nd sync char?
        beq     sync20          yes, continue

        lda     #$aa            send 2:nd sync char
        bra     sync04

sync16  ldx     #synstr         "Can't sync..."
sync17  jsr     pstrng
        jmp     warms           back to FLEX
*
sync20  ldx     #scnest         "Serial connection established"
        jsr     pstrng

*
*   Now do a "Where am I" command
*
        ldaa    #'?
        lbsr    schar
        bcc     sync16

*
*   Receive the current drive and folder string,
*   and keep the first letter, with some processing:
*   @ if floppy, other if hard disk
*
        lbsr    rchar
        bcc     sync16          exit if time-out
        pshs a                  save character
        suba    #1              A/B becomes @/A
        anda    #$5E            make upper case
        staa    drvltr          store it as @ if floppy
        cmpa    #$40            is it floppy?
        bne     wtack           no, leave as-is
        ldx     #$FFFF          set long delay
        stx     odelc           store it
*
*   receive all other characters and discard them
*   until the final ACK is received
*
wtack   lbsr    rchar
        bcc     sync16
        cmpa    #ack
        bne     wtack
*
*   Inform user about the current drive
*
        ldx     #drvmsg         point to string
        jsr     pstrng          print it
        puls    a               retrieve original char
        anda    #$5f            make upper case
        jsr     putchr          print it
        LDAA    #':             ... then print ":"
        jsr     putchr
*
*   Copy FLEX driver jump table to new location
*
        ldb     #7*3            number of bytes to move
        ldx     #$de00          start of FLEX table
        ldy     #$0000          new location

movtbl  lda     ,x+             read byte and move pointer
        sta     ,y+             store and move pointer
        decb                    decrement byte counter
        bne     movtbl
*
*   Move package to below current [MEMEND]
*
        ldaa    #$55            Point to a dummy drive number...
        staa    lstdrv          ... so that 'net drive'is not selected
*
        ldd     memend          make room for package
        subd    #drvend         end of package = byte count
        std     memend

        tfr     d,x             target pointer
        ldy     #$0000          start of package

movpkg  lda     ,y+             get one byte and move pointer
        sta     ,x+             store and move target pointer
        cmpy    #drvend         end of package?
        blo     movpkg          no, continue
*
*   Set new addresses in FLEX jump table
*
        ldx     #$de01          target pointer
        ldy     #newtbl         table of new jump addresses
        lda     #7              number of addresses to move
        sta     tries

movadr  ldd     ,y++            get address and move pointer
        addd    memend          add offset to address
        std     0,x             store at target
        leax    3,x             move target pointer
        dec     tries           decrement address counter
        bne     movadr

        ldd     memend          make sure memend points to free mem location
        subd    #$0001
        std     memend

        ldx     #instst         "Remote .DSK ...
        jsr     pstrng
        lda     netdrv          get 'net drive'#
        adda    #$30            make ASCII
        jsr     putchr
        jsr     pcrlf
        jmp     warms
*
* Messages to the user
*
greet   fcc     /FLEXNet driver version /
v1      fcb     0,'.,0,4
synstr  fcc     /Can't sync serial transfer!/,4
scnest  fcc     /Serial connection established/,4
instst  fcc     /Remote .DSK drive installed as drive #/,4
alread  fcc     /FLEXNet is already loaded, no action taken./,4
drvmsg  fcc     /Current MS-DOS drive is /,4
*
        end     start
