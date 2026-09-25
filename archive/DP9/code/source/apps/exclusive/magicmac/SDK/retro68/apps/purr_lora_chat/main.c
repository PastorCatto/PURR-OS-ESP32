/*
 * purr_lora_chat — minimal MagicMac app
 * Sends typed text over LoRa and displays received messages.
 * Compiled with Retro68 for Mac Plus / System 6.
 */
#include <MacTypes.h>
#include <QuickDraw.h>
#include <Windows.h>
#include <Dialogs.h>
#include <Events.h>
#include <Memory.h>
#include <string.h>
#include "../../PurrIPC.h"

#define WIN_W   400
#define WIN_H   220

static WindowRecord s_wRec;
static WindowPtr    s_win;

/* Circular receive log — 8 lines of 64 chars */
#define LOG_LINES   8
#define LOG_WIDTH   64
static char s_log[LOG_LINES][LOG_WIDTH];
static int  s_logHead = 0;

static void log_push(const char *line)
{
    strncpy(s_log[s_logHead % LOG_LINES], line, LOG_WIDTH - 1);
    s_log[s_logHead % LOG_LINES][LOG_WIDTH - 1] = '\0';
    s_logHead++;
}

static void draw_log(void)
{
    SetPort(s_win);
    EraseRect(&s_win->portRect);

    MoveTo(4, 14);
    DrawString("\pPURR LoRa Chat");

    for (int i = 0; i < LOG_LINES; i++) {
        int idx = (s_logHead - LOG_LINES + i + LOG_LINES) % LOG_LINES;
        Str255 ps;
        int len = strlen(s_log[idx]);
        ps[0] = (unsigned char)(len > 254 ? 254 : len);
        memcpy(&ps[1], s_log[idx], ps[0]);
        MoveTo(4, 30 + i * 14);
        DrawString(ps);
    }
}

static void send_message(const char *msg)
{
    char line[LOG_WIDTH];
    snprintf(line, sizeof(line), "TX: %s", msg);
    log_push(line);
    PurrLoRaSend(msg, (unsigned short)(strlen(msg)));
    draw_log();
}

void main(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0);
    InitCursor();

    Rect bounds = { 40, 40, 40 + WIN_H, 40 + WIN_W };
    s_win = NewWindow(&s_wRec, &bounds, "\pPURR LoRa Chat", true, noGrowDocProc,
                      (WindowPtr)-1L, true, 0);
    SetPort(s_win);
    draw_log();

    EventRecord ev;
    char        inputBuf[64];
    short       inputLen = 0;

    while (1) {
        /* Poll for incoming LoRa / notifications */
        PurrNotification notif;
        if (PurrNotifyPoll(&notif)) {
            char line[LOG_WIDTH];
            if (notif.type == kPurrNotifyLoRa) {
                snprintf(line, sizeof(line), "RX[%d]: %.*s",
                         notif.rssi, (int)notif.loraLen, (char *)notif.data);
            } else {
                snprintf(line, sizeof(line), "SYS: %s", (char *)notif.data);
            }
            log_push(line);
            draw_log();
        }

        /* Handle keyboard input — accumulate until Return */
        if (GetNextEvent(everyEvent, &ev)) {
            if (ev.what == keyDown) {
                char c = ev.message & charCodeMask;
                if (c == '\r' || c == '\n') {
                    if (inputLen > 0) {
                        inputBuf[inputLen] = '\0';
                        send_message(inputBuf);
                        inputLen = 0;
                    }
                } else if (c == '\b') {
                    if (inputLen > 0) inputLen--;
                } else if (inputLen < (short)(sizeof(inputBuf) - 1)) {
                    inputBuf[inputLen++] = c;
                }
            }
        }
    }
}
