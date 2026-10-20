# FlexNet client for FLEX 6800 (MC6800)
# Needs the Macro Assembler AS (asl, p2hex) and python3.

ASL      ?= asl
P2HEX    ?= p2hex
PYTHON   ?= python3
BUILD    ?= ../../build/6800

ASFLAGS  := -cpu 6800 -q
INC      := flex68.inc acia68.inc

.PHONY: all check clean

all: $(BUILD)/FNETDRV.CMD

$(BUILD)/%.p: %.asm $(INC)
	@mkdir -p $(BUILD)
	$(ASL) $(ASFLAGS) -L -olist $(BUILD)/$*.lst -o $@ $<

$(BUILD)/%.s19: $(BUILD)/%.p
	$(P2HEX) -q -F Moto -r '$$-$$' $< $@

$(BUILD)/FNETDRV.CMD: $(BUILD)/fnetdrv.s19
	$(PYTHON) s19cmd.py $< $@

# Package assembled $100 higher, for the relocation check
$(BUILD)/fnetdrv-hi.p: fnetdrv.asm $(INC)
	@mkdir -p $(BUILD)
	$(ASL) $(ASFLAGS) -D PKGORG=4352 -o $@ $<

check: $(BUILD)/fnetdrv.s19 $(BUILD)/fnetdrv-hi.s19
	$(PYTHON) relocchk.py $(BUILD)/fnetdrv.s19 $(BUILD)/fnetdrv.lst $(BUILD)/fnetdrv-hi.s19

clean:
	rm -f $(BUILD)/fnetdrv* $(BUILD)/FNETDRV.CMD
