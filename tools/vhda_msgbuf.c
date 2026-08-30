/* Read VoodooHDA internal message buffer via IOKit user client */
#include <stdio.h>
#include <stdlib.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

#define kVoodooHDAClassName        "VoodooHDADevice"
#define kVoodooHDAMemoryMessageBuffer  0x2000

int main(int argc, char **argv)
{
    io_service_t service;
    io_connect_t connect;
    kern_return_t kr;
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;

    service = IOServiceGetMatchingService(kIOMainPortDefault,
                IOServiceMatching(kVoodooHDAClassName));
    if (!service) {
        fprintf(stderr, "VoodooHDADevice not found\n");
        return 1;
    }

    kr = IOServiceOpen(service, mach_task_self(), 0, &connect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        return 1;
    }

    kr = IOConnectMapMemory64(connect, kVoodooHDAMemoryMessageBuffer,
                              mach_task_self(), &addr, &size,
                              kIOMapAnywhere | kIOMapReadOnly);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOConnectMapMemory64 failed: 0x%x\n", kr);
        IOServiceClose(connect);
        return 1;
    }

    /* Print the buffer contents (null-terminated string) */
    if (addr && size > 0) {
        const char *buf = (const char *)addr;
        /* If user wants grep-like filter */
        if (argc > 1) {
            const char *filter = argv[1];
            const char *p = buf;
            while (*p) {
                const char *eol = p;
                while (*eol && *eol != '\n') eol++;
                /* simple case-insensitive strstr */
                int found = 0;
                for (const char *s = p; s < eol && !found; s++) {
                    const char *a = s, *b = filter;
                    while (*b && a < eol) {
                        char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
                        char cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
                        if (ca != cb) break;
                        a++; b++;
                    }
                    if (!*b) found = 1;
                }
                if (found) {
                    fwrite(p, 1, eol - p, stdout);
                    fputc('\n', stdout);
                }
                p = *eol ? eol + 1 : eol;
            }
        } else {
            fwrite(buf, 1, strnlen(buf, (size_t)size), stdout);
        }
    }

    IOServiceClose(connect);
    return 0;
}
