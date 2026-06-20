/* Real open-source code through the ARET pipeline: Brad Conte's public-domain
   SHA-256 hashes a message; we print the hex digest. Pure integer/memory. */
#include "sha256.h"
__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) unsigned int strlen(const char *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int);

void __stdcall mainCRTStartup(void) {
    const char *msg = "ARET transpiles Windows to native Linux";
    BYTE hash[SHA256_BLOCK_SIZE];
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const BYTE *)msg, strlen(msg));
    sha256_final(&ctx, hash);
    printf("SHA256: ");
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) printf("%02x", hash[i]);
    printf("\n");
    ExitProcess(0);
}
