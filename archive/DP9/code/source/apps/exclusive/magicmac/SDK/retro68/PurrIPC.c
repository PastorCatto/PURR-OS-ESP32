/*
 * PurrIPC.c — Mac-side IPC implementation for MagicMac / PURR OS
 * Compiled with Retro68 (68k target).
 */
#include "PurrIPC.h"
#include <string.h>

static unsigned char s_seq = 0;

static PurrIPCFrame *ipc(void)
{
    return (PurrIPCFrame *)kPurrIPCBase;
}

/* Write command, spin-wait for DONE. Returns pointer to frame for reading response. */
static PurrIPCFrame *_call(unsigned char cmd, const void *payload, unsigned short payloadLen)
{
    PurrIPCFrame *f = ipc();

    /* Wait until ESP32 is not busy (should be near-instant) */
    while (f->status == kIPCStatusBusy) {}

    f->cmd        = cmd;
    f->seq        = ++s_seq;
    f->responseLen = 0;
    f->payloadLen  = payloadLen;
    if (payloadLen && payload)
        memcpy(f->payload, payload, payloadLen);

    /* Commit: writing status last makes it visible to ESP32 atomically */
    f->status = kIPCStatusPending;

    /* Spin until done — ESP32 services this in < 1 ms normally */
    while (f->status != kIPCStatusDone && f->status != kIPCStatusError) {}

    return f;
}

void PurrLoRaSend(const void *data, unsigned short len)
{
    _call(kIPCCmdLoRaSend, data, len);
}

short PurrNotifyPoll(PurrNotification *out)
{
    PurrIPCFrame *f = _call(kIPCCmdNotifyPending, 0, 0);
    if (f->responseLen == 0) return 0;

    unsigned char tag = f->response[0];
    if (tag == 0x01) {
        /* LoRa packet: [0x01][rssi:4 bytes][data...] */
        out->type = kPurrNotifyLoRa;
        memcpy(&out->rssi, &f->response[1], sizeof(short));
        out->loraLen = (unsigned char)(f->responseLen - 1 - sizeof(int));
        if (out->loraLen > 127) out->loraLen = 127;
        memcpy(out->data, &f->response[1 + sizeof(int)], out->loraLen);
    } else {
        /* Text notification */
        out->type = kPurrNotifyText;
        unsigned short len = f->responseLen;
        if (len > 127) len = 127;
        memcpy(out->data, f->response, len);
        out->data[len] = '\0';
    }
    return 1;
}

void PurrNotifyPost(const char *text)
{
    _call(kIPCCmdNotifyPost, text, (unsigned short)(strlen(text) + 1));
}

void PurrWiFiGetStatus(PurrWiFiStatus *out)
{
    PurrIPCFrame *f = _call(kIPCCmdWiFiStatus, 0, 0);
    if (f->responseLen >= sizeof(PurrWiFiStatus))
        memcpy(out, f->response, sizeof(PurrWiFiStatus));
}

void PurrWiFiConnect(const char *ssid, const char *password)
{
    /* payload: ssid\0password\0 */
    unsigned char buf[128];
    unsigned short sl = (unsigned short)(strlen(ssid) + 1);
    unsigned short pl = (unsigned short)(strlen(password) + 1);
    memcpy(buf, ssid, sl);
    memcpy(buf + sl, password, pl);
    _call(kIPCCmdWiFiConnect, buf, sl + pl);
}

void PurrWiFiScan(char *buf, unsigned short bufLen)
{
    PurrIPCFrame *f = _call(kIPCCmdWiFiScan, 0, 0);
    unsigned short n = f->responseLen < bufLen ? f->responseLen : (unsigned short)(bufLen - 1);
    memcpy(buf, f->response, n);
    buf[n] = '\0';
}

void PurrBTList(char *buf, unsigned short bufLen)
{
    PurrIPCFrame *f = _call(kIPCCmdBTList, 0, 0);
    unsigned short n = f->responseLen < bufLen ? f->responseLen : (unsigned short)(bufLen - 1);
    memcpy(buf, f->response, n);
    buf[n] = '\0';
}
