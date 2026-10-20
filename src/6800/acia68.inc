;*-------------------------------------------------------------------
;* acia68.inc - FlexNet serial port (system-specific)
;*
;*    The 6850 on an SWTPC MP-S board. Each I/O slot is 4 bytes at
;*    $8000; control/status = base, data = base+1. Slot 1 is the
;*    console. Override the slot with: asl -D ACIAPORT=n
;*-------------------------------------------------------------------
	ifndef ACIAPORT
ACIAPORT equ 2		;* I/O slot of the FlexNet MP-S board
	endif
aciac	equ  $8000+4*ACIAPORT ;* ACIA control/status register
aciad	equ  aciac+1	;* ACIA data register
