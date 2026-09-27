/* hello.c - Simple "HELLO WORLD" COM program for MagiDOS (C version)
   Compile with: wcl -mt -0 -os hello.c -fe=hello.com

   -mt = tiny memory model (COM file format)
   -0  = 8086 CPU only
   -os = optimize for size
*/

#include <stdio.h>

int main(void)
{
    printf("HELLO WORLD FROM C!\n");
    printf("This is a simple DOS program running under MagiDOS.\n");
    printf("Press any key to exit...\n");

    getch();  /* Wait for keypress */
    return 0;
}
