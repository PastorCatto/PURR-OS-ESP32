; PurrDOS.asm — PURR OS kernel INT 0E0h stubs
; Assemble with: nasm -f obj PurrDOS.asm -o PurrDOS.obj
; Link with your OpenWatcom project: wlink ... file PurrDOS.obj

PURR_INT    EQU 0E0h

segment _TEXT public class=CODE use16

; void PurrLoRaSend(void *buf, unsigned cx);
; AH=10h, DS:SI=buf, CX=len
global _PurrLoRaSend
_PurrLoRaSend:
    push    bp
    mov     bp, sp
    push    ds si cx
    mov     ah, 10h
    lds     si, [bp+4]      ; far ptr buf
    mov     cx, [bp+8]      ; len
    int     PURR_INT
    pop     cx si ds
    pop     bp
    ret

; unsigned PurrLoRaRecv(void *buf, unsigned cx);
; AH=11h, ES:DI=buf, CX=buflen -> AX=bytes (0 if none)
global _PurrLoRaRecv
_PurrLoRaRecv:
    push    bp
    mov     bp, sp
    push    es di cx
    mov     ah, 11h
    les     di, [bp+4]
    mov     cx, [bp+8]
    int     PURR_INT
    pop     cx di es
    pop     bp
    ret                     ; AX = byte count from emulator

; void PurrWiFiStatus(PurrDOSWiFiStatus *out);
; AH=20h, ES:DI=out
global _PurrWiFiStatus
_PurrWiFiStatus:
    push    bp
    mov     bp, sp
    push    es di
    mov     ah, 20h
    les     di, [bp+4]
    int     PURR_INT
    pop     di es
    pop     bp
    ret

; void PurrWiFiConnect(const char *ssid_pass_buf);
; AH=22h, DS:SI=ssid\0pass\0
global _PurrWiFiConnect
_PurrWiFiConnect:
    push    bp
    mov     bp, sp
    push    ds si
    mov     ah, 22h
    lds     si, [bp+4]
    int     PURR_INT
    pop     si ds
    pop     bp
    ret

; void PurrNotifyPost(const char *text);
; AH=40h, DS:SI=text
global _PurrNotifyPost
_PurrNotifyPost:
    push    bp
    mov     bp, sp
    push    ds si
    mov     ah, 40h
    lds     si, [bp+4]
    int     PURR_INT
    pop     si ds
    pop     bp
    ret

; unsigned char PurrNotifyPoll(PurrDOSNotif *out);
; AH=41h, ES:DI=out -> ZF clear if notification present
global _PurrNotifyPoll
_PurrNotifyPoll:
    push    bp
    mov     bp, sp
    push    es di
    mov     ah, 41h
    les     di, [bp+4]
    int     PURR_INT
    mov     al, 0
    jz      .none
    mov     al, 1
.none:
    pop     di es
    pop     bp
    ret
