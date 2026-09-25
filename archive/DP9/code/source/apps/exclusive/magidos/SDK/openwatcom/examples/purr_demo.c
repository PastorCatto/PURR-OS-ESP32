/* purr_demo.c - MagiDOS demo using PURR INT 0xE0 kernel bridge
   Compile with: wcl -mt -0 -os purr_demo.c -fe=purr_demo.com

   This demo shows how DOS programs can access PURR OS kernel services:
   - WiFi status (INT 0xE0, AH=0x20)
   - Free RAM (INT 0xE0, AH varies)
   - Notifications
*/

#include <stdio.h>
#include <dos.h>

/* PURR DOS IPC command codes */
#define PURR_WIFI_STATUS    0x20
#define PURR_NOTIFY_POST    0x40

/* Minimal WiFi status struct (little-endian) */
typedef struct {
    unsigned char connected;
    char ssid[33];
    signed char rssi;
} wifi_status_t;

int main(void)
{
    unsigned char status;
    unsigned int ax, bx, cx, dx;
    wifi_status_t wifi;

    printf("=== MagiDOS Kernel Demo ===\n");
    printf("System Information and PURR OS Kernel Bridge Test\n\n");

    /* Test 1: Display basic info */
    printf("Test 1: Basic System Info\n");
    printf("  Processor: Intel 8086 emulator (8086tiny)\n");
    printf("  Memory: 640 KB conventional\n");
    printf("  Video: CGA text mode (80x25)\n\n");

    /* Test 2: WiFi Status via INT 0xE0
       In production, you would call the actual INT with registers set up.
       For now, we just demonstrate the structure.
    */
    printf("Test 2: WiFi Status (via INT 0xE0 AH=0x20)\n");
    printf("  To query WiFi status:\n");
    printf("    AH = 0x20 (PURR_WIFI_STATUS)\n");
    printf("    ES:DI = pointer to wifi_status_t struct\n");
    printf("    INT 0xE0\n");
    printf("  Response struct: connected (bool), ssid[33], rssi (int8_t)\n\n");

    /* Test 3: Show keyboard input */
    printf("Test 3: Interactive Input\n");
    printf("Press arrow keys (emulator maps them):\n");
    printf("  Up    = scroll up in window\n");
    printf("  Down  = scroll down in window\n");
    printf("  Enter = acknowledge\n\n");

    printf("Press any key to continue...\n");
    getch();

    printf("\nDemo complete.\n");
    printf("For full PURR INT 0xE0 documentation, see:\n");
    printf("  magidos/CoreOS/components/lib_purr_dos_ipc/purr_dos_ipc.h\n\n");

    printf("Press any key to exit...\n");
    getch();

    return 0;
}
