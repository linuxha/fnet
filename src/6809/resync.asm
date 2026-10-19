*   File name:  resync.asm
*
*   This utility will try to re-synchronize the communication
*   between the FLEX/netdrv and the remote host computers.
*
*   Vn  01.00   2000-09-23, BjB.
*       02.00   2002-05-01, js, use send/receive vectors
*       02.01   2002-08-31  js Use signature string
*       02.02   2002-09-19  js restore ACIA reset routine
* --------------------------------------------------------------
*
*   FLEX equates.
*
warms   equ     $cd03           FLEX warm start
pstrng  equ     $cd1e           write string to display
pcrlf   equ     $cd24           write cr/lf to display
memend  equ     $cc2b           memory end pointer
*
* ---------------------------------------------------------------
*
        org     $c100

start   bra     init

versn   FCB     2,2             version number
tries   rmb     1               sync tries counter
tmp     rmb     1               temporary storage for sync char
*
* ---------------------------------------------------------------
*

* Serial input/output vectors
* The following 2 JMPs are initialized
* with the addresses of the serial in/out
* vectors which are at the start of NETDRV
* The default (WARMS) is a dummy value
*
schar JMP warms
rchar JMP warms
reset JMP warms
*
init equ *   Start of code

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
 leax 3,x
 stx reset+1

*   acia reset
*
 bsr reset

*   Main routine.
*
        jsr     pcrlf
        lda     #$5             number of tries
        sta     tries
        lda     #$55            1:st sync character

sync    sta     tmp             current sync character

sync04  lbsr    schar           send character
        bcc     sync16          time-out, report error

        lbsr    rchar           get response
        bcc     sync16          time-out, report error

        cmpa    tmp             received char same as sent?
        beq     sync08          yes
        lda     tmp             1:st sync char?
        cmpa    #$55
        bne     sync20          nope, report error

        dec     tries           decrement try count
        bne     sync04          try again if not = 0
        bra     sync20          report error

sync08  cmpa    #$aa            2:nd sync character?
        beq     sync12          yes, report success
        ldaa    #$aa            send 2:nd sync character
        bra     sync
*
sync12  ldx     #succst         "Connection successfully.."
        bra     sync24

sync16  ldx     #timest         "Time-out.."
        bra     sync24

noload  ldx #nodrv  Inform that drivers are not loaded
        bra sync24

sync20  ldx     #synest         "Could not sync.."
sync24  jsr     pstrng
        jmp     warms           back to FLEX
*
* ---------------------------------------------------------------
*
succst  fcc     /Connection successfully established/,4
timest  fcc     /Time-out error, connection broken!/,8,4
synest  fcc     /Could not synchronize the communication!/,8,4
nodrv FCC /NETDRV is not loaded in memory, no action taken./,4

* signature string
sgnst fcc 'netUUdrv'
len equ *-sgnst

*
* ---------------------------------------------------------------
*
*
        end     start
*
