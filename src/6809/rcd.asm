*   File name:  RCD.TXT
*
*   This utility will Change to a new Directory (hence CD)
*   on a remote host computer, that is running NETPC.
*   netdrv must already be running on the FLEX computer.
*
*   Syntax: RCD <path>
*
*   Note: <path> may contain the whole directory path,
*         including the disk name, for example "C:\dir1\dir2\.."
*         In order to achieve this, the FLEX EOL character is
*         temporarily cleared. Thus, multiple commands on one
*         line can be used with this utility, only if RMOUNT
*         is the last command on the line.
*
*   Adapted from Bjarne Backstrom's source code for RMOUNT
*   by Joel Setton
*
*       02.01   2002-08-31 js Use signature string
*       02.02   2002-09-19 js New vectors
* ---------------------------------------------------------------
*
*   FLEX equates.
*
warms   equ     $cd03           FLEX warm start
pstrng  equ     $cd1e           write string to display
pcrlf   equ     $cd24           write cr/lf to display
nxtch   equ     $cd27           get next buffer character
memend  equ     $cc2b           memory end pointer
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


start EQU *
        bra     COLD

versn   fcb     2,2             version number
eoltmp  rmb     1               temp storage for EOL character
chrcnt  rmb     1               character counter
*
* ---------------------------------------------------------------
* Serial input/output vectors
* The following 2 JMPs are initialized
* with the addresses of the serial in/out
* vectors which are at the start of NETDRV
* The default (warms) is a dummy value
*
schar JMP warms
rchar JMP warms
*
COLD EQU * START OF CODE
*
* Scan memory from MEMEMD to $C000 to find
* out if a copy of NETDRV is already loaded
*
search ldx memend  start of search
sear2 leax 1,x Bump pointer
 cpx #$c000 Finished?
 lbeq noload Yes, not found
 ldy #sgnst Point to target string
 clr b Reset byte counter
sear3 ldaa b,x Get byte from RAM
 cmpa b,y Same as signature?
 bne sear2 No, bump and restart
 inc b Point to next byte
 cmpb #len Finished?
 bne sear3 No, check next byte
*
* string was found, all is OK

* Set the address of the in/out vectors
 leax len,x
 stx schar+1
 leax 3,x
 stx rchar+1
*
* ---------------------------------------------------------------
*
*
        jsr     pcrlf

        lda     ttyeol          save EOL character
        sta     eoltmp

        clr     chrcnt          check file name length
        ldx     ttylbp

chec04  lda     ,x+             get character
        cmpa    #$20            skip leading spaces
        beq     chec04

chec08  cmpa    #cr             carriage return?
        beq     chec12
        inc     chrcnt          no, inc character count
        lda     ,x+             check next character
        bra     chec08

chec12  lda     chrcnt          check character count
        cmpa    #2              less than 2 characters?
        lblo    badfnm          yes, report bad file name
*
* ---------------------------------------------------------------
*
        clr     ttyeol          disable TTYEOL

        lda     #'P             send P (Point) command to remote host
        lbsr    schar
        lbcc    nwrkng          time out, communication not working
*
main04  jsr     nxtch           skip leading spaces
        cmpa    #$20
        beq     main04

main08  lbsr    schar           send one character to remote host
        lbcc    nwrkng          time out, communication not working
        cmpa    #cr             last character in line?
        beq     main12
        jsr     nxtch           get next character
        bra     main08
*
main12  lbsr    rchar           get response
        lbcc    nwrkng          time out, communication not working
        cmpa    #ack            got an ack?
        lbne    badfnm          no, report bad file name
        ldx     #succst         report success
        bra     finish
*
* ---------------------------------------------------------------
*
nwrkng  ldx     #nwrkst         communication is not working
        bra     finish

noload   ldx #nodrv  Inform that drivers are not loaded
         bra finis2

badfnm  ldx     #badfst         bad file name

finish  lda     eoltmp          restore EOL character
        sta     ttyeol
finis2 jsr     pstrng          print string pointed to by XREG
        jmp     warms           back to FLEX
*
* ---------------------------------------------------------------
*
nwrkst  fcc     /Communication is not working!/,8,4
badfst  fcc     /Bad directory name!/,8,4
succst  fcc     /Command executed OK./,4
nodrv   FCC     /NETDRV is not loaded in memory, no action taken./,4

* signature string
sgnst fcc 'netUUdrv'
len equ *-sgnst

 end start

