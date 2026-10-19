*   File name  fmount.asm
*
*   This utility will "MOUNT" a .DSK file
*   on a remote host computer, that is running NETPC.
*   netdrv must already be running on the FLEX computer.
*
*   Syntax: FMOUNT <drive number> [<file_name>]
*
*   Note: <file_name> may contain the whole directory path,
*         including the disk name, for example "C:\dir1\dir2\.."
*         In order to achieve this, the FLEX EOL character is
*         temporarily cleared. Thus, multiple commands on one
*         line can be used with this utility, only if RMOUNT
*         is the last command on the line.
*
*   Vn  01.00   2000-09-25, BjB.
*       01.01   2002-05-01 js Use the serial routines in NETDRV
*       02.01   2002-08-31 js Use signature string
*       02.02   2002-09-19 js New vectors
*       02.03   2002-10-06 js Check for proper extension
*       02.04   2002-11-12 js Add read/write status check

* ---------------------------------------------------------------
*
*   FLEX equates.
*
warms   equ     $cd03           FLEX warm start
pstrng  equ     $cd1e           write string to display
pcrlf   equ     $cd24           write cr/lf to display
nxtch   equ     $cd27           get next buffer character
MEMEND  EQU     $CC2B
*
ttylbp  equ     $cc14           line buffer pointer location
ttyeol  equ     $cc02           end of line character location
*
* ---------------------------------------------------------------
*
ack     equ     $06             acknowledge character
cr      equ     $0d             carriage return character

*
* ***************************************************************
*
        org     $c100
start   BRA     COLD
*
*
versn   fdb     $0204           version number
eoltmp  rmb     1               temp storage for EOL character
chrcnt  rmb     1               character counter
fnprsnt fcb     0               assume we are talking to NetPC
drv     fcb     0
ndarray fdb     0

* Serial input/output vectors
* The following 2 JMPs are initialized
* with the addresses of the serial in/out
* vectors which are at the start of NETDRV
* The default (WARMS) is a dummy value

schar   JMP     warms
rchar   JMP     warms


* START OF CODE

COLD    EQU     *

*
* Scan memory from MEMEMD to $C000 to find
* out if a copy of NETDRV is already loaded
*
search  ldx     MEMEND          start of search
sear2   leax    1,x             Bump pointer
        cpx     #$c000          Finished?
        lbeq    noload          Yes, not found
        ldy     #sgnst          Point to target string
        clr     b               Reset byte counter

sear3   ldaa    b,x             Get byte from RAM
        cmpa    b,y             Same as signature?
        bne     sear2           No, bump and restart
        inc     b               Point to next byte
        cmpb    #len            Finished?
        bne     sear3           No, check next byte

* string was found, all is OK

* Set the address of the in/out vectors

        leax    len,x           point to schar routine
        stx     schar+1
        leax    3,x             point to rchar routine
        stx     rchar+1

* See if we are talking to FNETDRV. Y is still pointing
* at the first character of FLEXNet if it's there.

        leax    3,x             point at the reset vector
        leax    3,x             point at the FLEXNet 4.1.0 string

        ldy     #fnsgnt         Point to target string
        clr     b
sear5   ldaa    b,x
        cmpa    b,y
        bne     check           Not FNETDRV
        inc     b
        cmpb    #len2
        bne     sear5

* X is still pointing at the FLEXNet 4.1.0 string. We need
* to add B to it to get it to point at rge drvltr table.
* If we add 4 more to it we will be pinting at the netdrv array.

        abx                     add B to X to point at drvltrs
        leax    4,x             now pointing at the netdrv array
        stx     ndarray

        ldaa    #1
        staa    fnprsnt
*
* ---------------------------------------------------------------
*
check   jsr     pcrlf

        lda     ttyeol          save EOL character
        sta     eoltmp

        lda     #'Q             quick check that communication is working
        lbsr    schar
        lbcc    nwrkng          time out, communication not working

        lbsr    rchar           get response
        lbcc    nwrkng          time out, communication not working
        cmpa    #ack            got an ack?
        lbne    nwrkng          communication not working

*
* -----------------------------------------------------
*
*  Check file name length/extension

        clr     chrcnt          no character input yet
        ldx     ttylbp          point to TTY Line Buffer

chec04  lda     ,x+             get character
        cmpa    #$20            skip leading spaces
        beq     chec04          loop skipping spaces

        cmpa    #cr             see if empty line
        lbeq    shwmap          just show the current map

chec08  cmpa    #cr             carriage return?
        beq     chec12
        cmpa    #'.             Extension provided?
        bne     chec10

* the next characters must be "dsk"

        ldy     #ext

chec09  lda     ,x+
        anda    #$5f
        cmpa    ,y+
        lbne    badex
        cmpy    #ext+3
        bne     chec09

chec10  inc     chrcnt          no, inc character count
        pshs    a
        lda     chrcnt          check character count
        cmpa    #1              just a drive specified?
        puls    a               does not change cc reg
        bne     chec11

        anda    #$03
        staa    drv

chec11  lda     ,x+             check next character
        bra     chec08

chec12  lda     fnprsnt
        beq     chec13          skip next if not using FLEXNet

        lda     chrcnt          check character count
        cmpa    #1
        bne     chec13

        ldx     ndarray
        ldab    drv
        ldaa    #-1
        staa    b,x

        ldx     #unmapp         unmapped message
        jsr     pstrng
        bra     main

chec13  lda     fnprsnt
        beq     chec15

        ldx     ndarray         map the drive as redirected
        ldab    drv
        cmpb    #3              will be > 3 if no drive specified
        bgt     chec14          because it will be a letter
        stab    b,x
        bra     chec15

chec14  ldab    #3              if user does not supply drive
        stab    b,x             assume drive 3 requested

chec15  lda     chrcnt
        cmpa    #2              less than 2 characters?
        lblo    badfnm          yes, report bad file name
*
* ---------------------------------------------------------------
*
main    clr     ttyeol          disable TTYEOL

        lda     #'m             send m(ount) command to remote host
        lbsr    schar
        lbcc    nwrkng          time out, communication not working
*
main04  jsr     nxtch           skip leading spaces
        cmpa    #$20
        beq     main04

        anda    #$03

main08  cmpa    #$20
        bne     main09
        jsr     nxtch
        bra     main08

main09  lbsr    schar           send one character to remote host
        lbcc    nwrkng          time out, communication not working
        cmpa    #cr             last character in line?
        beq     main12
        jsr     nxtch           get next character
        cmpa    #'.             substitute cr for dot
        bne     main08
        ldaa    #cr
        bra     main08
*
main12  lbsr    rchar           get response
        lbcc    nwrkng          time out, communication not working
        cmpa    #ack            got an ack?
        lbne    badfnm          no, report bad file name
*
* Check for "R" or "W" after the ack
        lbsr    rchar           get character
        lbcc    nwrkng          time out, not working
        cmpa    #'R             Read only?
        beq     read
        cmpa    #'W             Write only?
        beq     write
        bra     badfnm          otherwise, report error

*
* ---------------------------------------------------------------
badex   ldx     #exten          Bad extension
        bra     finish

noload  ldx     #nodrv          Driver not found
        bra     finish

read    ldx     #readst         Read-only message
        bra     finish

write   ldx     #writest        Full access message
        bra     finish

nwrkng  ldx     #nwrkst         communication is not working
        bra     finish
*
badfnm  ldx     #badfst         bad file name
*
finish  lda     chrcnt
        cmpa    #1
        beq     alldone

        jsr     pstrng          print string pointed to by XREG

alldone lda     eoltmp          restore EOL character
        sta     ttyeol
        jmp     warms           back to FLEX

* User entered command with NO parameters - Show mount map

shwmap  ldx     ndarray       point to drive array
        ldb     #4            set count to 4

chkmap  lda     0,x           get
        stx     xtemp1
        cmpa    #$FF          see if mapped
        beq     nomap         drive not mapped to FLEXNet

        pshs    a,b
        ldx     #mapped       show mapped
        adda    #$30          make drive number ascii
        sta     6,x

        puls    a,b          restore the counter and the drive #
        pshb                 save the counter again

        jsr     shwdsk       show the image file name (a = drive)

        pulb                 restore the counter
        ldx     xtemp1       restore the poiinter

nomap   leax    1,x
        decb
        bne     chkmap

        jsr     pcrlf

        bra     alldone

*
* ---------------------------------------------------------------
*
shwdsk  ldx     #dsknam
        stx     xtemp2

        psha                 save drive number in Areg
        lda     #'d          send 'd'sk command
        lbsr    schar
        lbcc    nwrkng       error

        pula                 get drive number
        lbsr    schar        send it also
        lbcc    nwrkng       error

sdloop  lbsr    rchar        get character from HOST
        cmpa    #ack         are we done?
        beq     sdprnt       show user what we got

        ldx     xtemp2       get pointer to where we are saving
        sta     0,x
        lda     #4
        leax    1,x
        sta     0,x          terminate string on each character
        stx     xtemp2
        bra     sdloop

sdprnt  ldx     #mapped
        jsr     pstrng
        rts
*
* ---------------------------------------------------------------
*

xtemp1  fdb     0
xtemp2  fdb     0
mapped  fcc     /Drive   -> /
dsknam  rmb     256
exten   fcc     /Illegal file extension/,4
nwrkst  fcc     /Communication is not working!/,4
badfst  fcc     /Could not open file/,4
readst  fcc     /File open in read-only mode/,4
writest fcc     'File opened with full access (read/write)',4
unmapp  fcc     /Drive unmapped/,4

succst  fcc     /Command executed OK./,4
nodrv   fcc     /NETDRV is not loaded in memory, no action taken./,4

* signature string
sgnst fcc 'netUUdrv'
len equ *-sgnst

fnsgnt fcc 'FLEXNet 4.1.0'
len2 equ *-fnsgnt

* Extension string
ext fcc /DSK/

        end     start
