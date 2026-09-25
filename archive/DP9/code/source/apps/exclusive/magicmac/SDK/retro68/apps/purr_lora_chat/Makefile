APP     = purr_lora_chat
SRCS    = main.c ../../PurrIPC.c
RETRO68 = $(shell which RetroCC)

$(APP).dsk: $(APP).bin
	hformat -l $(APP) $(APP).dsk
	hcopy -r $(APP).bin :$(APP)
	hattrib -c 'APPL' -t '????' :$(APP)

$(APP).bin: $(SRCS)
	$(RETRO68) -o $@ $(SRCS) -I../../

clean:
	rm -f $(APP).bin $(APP).dsk
