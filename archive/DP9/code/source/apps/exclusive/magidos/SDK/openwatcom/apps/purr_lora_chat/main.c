/*
 * purr_lora_chat.c — minimal MagiDOS app
 * DOS text-mode LoRa chat terminal.
 * Compile: wcl -mt -0 -os main.c ..\..\PurrDOS.obj -fe=purr_lora_chat.com
 */
#include <dos.h>
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include "../../PurrDOS.h"

#define SCREEN  ((unsigned short far *)0xB8000000L)
#define COLS    80
#define ROWS    25
#define ATTR    0x0700  /* light grey on black */

static void cls(void)
{
    int i;
    for (i = 0; i < COLS * ROWS; i++) SCREEN[i] = ATTR | ' ';
}

static void puts_at(int row, int col, const char *s, unsigned char attr)
{
    unsigned short a = (unsigned short)attr << 8;
    int i;
    for (i = 0; s[i] && col + i < COLS; i++)
        SCREEN[row * COLS + col + i] = a | (unsigned char)s[i];
}

/* Scroll rows 1..ROWS-2 up one line, clear last row */
static char s_log[22][80];
static int  s_nlog = 0;

static void log_push(const char *line)
{
    if (s_nlog < 22) {
        strncpy(s_log[s_nlog++], line, 79);
    } else {
        int i;
        for (i = 0; i < 21; i++) memcpy(s_log[i], s_log[i+1], 80);
        strncpy(s_log[21], line, 79);
    }
}

static void redraw(const char *input)
{
    int i;
    cls();
    puts_at(0, 0, "MagiDOS LoRa Chat  (Enter=send, ESC=quit)", 0x0F);
    for (i = 0; i < s_nlog; i++)
        puts_at(1 + i, 0, s_log[i], 0x07);
    puts_at(23, 0, "> ", 0x0A);
    puts_at(23, 2, input, 0x0F);
}

void main(void)
{
    char input[78];
    int  ilen = 0;
    PurrDOSNotif notif;

    cls();
    puts_at(0, 0, "MagiDOS LoRa Chat", 0x0F);
    puts_at(2, 0, "Loading...", 0x07);

    input[0] = '\0';
    redraw(input);

    for (;;) {
        /* Poll for incoming LoRa / notifications */
        if (PurrNotifyPoll(&notif)) {
            char line[80];
            if (notif.type == PURR_NOTIF_LORA) {
                _snprintf(line, 80, "RX[%d dBm]: %.*s",
                          (int)notif.rssi, (int)notif.len, (char *)notif.data);
            } else if (notif.type == PURR_NOTIF_TEXT) {
                _snprintf(line, 80, "SYS: %s", (char *)notif.data);
            } else {
                line[0] = '\0';
            }
            if (line[0]) { log_push(line); redraw(input); }
        }

        if (!kbhit()) continue;

        int c = getch();
        if (c == 27) break;  /* ESC — exit */

        if (c == '\r') {
            if (ilen > 0) {
                char line[80];
                input[ilen] = '\0';
                _snprintf(line, 80, "TX: %s", input);
                log_push(line);
                PurrLoRaSend(input, (unsigned short)ilen);
                ilen = 0;
                input[0] = '\0';
                redraw(input);
            }
        } else if (c == '\b') {
            if (ilen > 0) { ilen--; input[ilen] = '\0'; redraw(input); }
        } else if (c >= 0x20 && ilen < 77) {
            input[ilen++] = (char)c;
            input[ilen]   = '\0';
            redraw(input);
        }
    }

    cls();
}
