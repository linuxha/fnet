        NAM     REXIT
*
* "RemoteEXIT" command does the following:
*
* - Exit NETPC which is running on the PC
* - Reset the Flex disk driver vectors to their
*   original values
* - Clear (i.e. fill with zeroes) the space used
*   by the NETDRV drivers
* - Reset MEMEND to recover the free memory space,
*   but do so ONLY if the NETDRV code was located
*   immediately above MEMEND.
*
*
*       01.01   2002-11-29 js Initial version
* ---------------------------------------------------------------

        LIB     FLEXEQU

        ORG     $C100
        BRA     search

* VERSION NUMBER

VN      FCB     1,1

* TEMP STORAGE

base    RMB     2               start of signature string
size    RMB     2               size of driver code

* Serial input/output vectors
* The following 2 JMPs are initialized
* with the addresses of the serial in/out
* vectors which are at the start of NETDRV
* The default (WARMS) is a dummy value

schar   JMP     WARMS
rchar   JMP     WARMS

memend  equ     $cc2b           FLEX end of user RAM
*

*-----------------------------------------
* Step 0: Check for Netpc drivers in memory; if
*         they are not loaded, just exit.
*
* Scan memory from MEMEMD to $C000 to find
* out if a copy of NETDRV is loaded
*
search  ldx     memend          start of search
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

* Signature string was found, all is OK
* Proceed with utility!

*--------------------------------------
*
* Step 1: Store start and end pointers
*

* Store drivers start address

        tfr     x,d
        subd    #21             point to start of code
        std     base            store pointer

* Set the address of the in/out vectors

        leax    len,x
        stx     schar+1
        leax    3,x
        stx     rchar+1

* Read size of drivers and store it

        ldd     8,x
        std     size

*----------------------------------
*
* Step 2: Send the "Exit" command

        LDA     #'E
        JSR     schar           send command
        BCC     NOWORK          exit if time-out

* WAIT FOR ACK

WTACK   JSR     rchar           Receive character
        BCC     NOWORK          exit if time-out
        CMPA    #ACK            "ack" received?
        BNE     WTACK           No, try again

*----------------------------------
*
* Step 3: Restore the original Flex
*         disk driver vectors

clear   ldab    #21             Restore 7 vectors
        ldx     base "from"     pointer
        ldy     #$de00 "to"     pointer

* restore loop

relp    ldaa    0,x+            read from Net drivers
        staa    0,y+            Store in Flex area
        decb
        bne     relp            loop until done


*---------------------------------
*
* Step 4: Fill driver space with zeroes
*

        ldx     base            Point to base address
        ldd     size            Number of bytes to clear
fill    clr     0,x+            clear one byte
        subd    #1              count down
        bne     fill            loop until done

*----------------------------------
*
* Step 5: If the drivers were loaded right
*         above MEMEMD, restore it;
*         otherwise exit.

        ldd     memend          Address of last free byte
        addd    #1              Bump to first used
        cmpd    base            Are they equal?
        bne     EXITOK          No, exit without restoring MEMEND

* Restore MEMEND

        addd    size            Add size of drivers
        std     memend          ... and restore

        ldx     #restor         Go inform user
        bra     finish          ... and exit


*------------------------------------
* Message and exit routines

noload  ldx     #nodrv          Inform that drivers are not loaded
        bra     finish

EXITOK  ldx     #exitst         Report exit from program
        bra     finish

NOWORK  LDX     #TIMOUT         Report time-out error

finish  JSR     PSTRNG
EXIT2   JMP     WARMS


* CHARACTER STRINGS

TIMOUT  FCC     /Communication time-out error/,4
nodrv   FCC     /NETDRV is not loaded in memory, no action taken./,4
restor  fcc     /MEMEMD restored/,CR,LF
exitst  fcc     /Program Exit/,4

* signature string

sgnst   fcc     'netUUdrv'
len     equ     *-sgnst


 END $C100
