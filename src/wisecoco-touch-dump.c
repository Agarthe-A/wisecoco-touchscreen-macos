/*
 * wisecoco-touch-dump - print and decode the HID input reports of the
 * Wisecoco 12.3" bar touch controller.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 wisecoco-touchscreen-macos contributors
 * See the LICENSE file for the full license text.
 *
 * Report ID 4 (multitouch digitizer, 10 contacts) is decoded per contact.
 * Report ID 5 (mouse emulation) is summarised; all-zero report 5 packets
 * mean the controller is in its broken macOS fallback mode and
 * wisecoco-touch-enable has to be run.
 *
 * Permission: reading input reports of a pointing device requires Input
 * Monitoring for the app that runs this tool (Terminal, iTerm, ...):
 * System Settings > Privacy & Security > Input Monitoring. Without it,
 * IOHIDManagerOpen() returns kIOReturnNotPermitted (0xe00002e2).
 *
 * Build:
 *   clang -O2 -Wall -o wisecoco-touch-dump wisecoco-touch-dump.c \
 *         -framework IOKit -framework CoreFoundation
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>

#define TOOL_NAME        "wisecoco-touch-dump"

#define DEFAULT_VID      0x0712
#define DEFAULT_PID      0x0010
#define DEFAULT_SECONDS  30

#define REPORT_BUF_SIZE  1024
#define TOUCH_REPORT_ID  4
#define MOUSE_REPORT_ID  5
#define MAX_CONTACTS     10
#define CONTACT_SIZE     5
/* Report 4 without its ID byte: 10 contacts, scan time (4), count (1). */
#define TOUCH_PAYLOAD    (MAX_CONTACTS * CONTACT_SIZE + 4 + 1)
#define MOUSE_ZERO_EVERY 100

static unsigned g_vid = DEFAULT_VID;
static unsigned g_pid = DEFAULT_PID;
static CFAbsoluteTime g_t0;
static unsigned long g_devices, g_touch, g_mouse, g_mouse_zero, g_other;

static double elapsed(void)
{
    return CFAbsoluteTimeGetCurrent() - g_t0;
}

static void hex_dump(const uint8_t *p, CFIndex n, char *out, size_t outsz)
{
    CFIndex shown = n < 24 ? n : 24;
    size_t pos = 0;

    out[0] = '\0';
    for (CFIndex i = 0; i < shown && pos + 4 < outsz; i++)
        pos += (size_t)snprintf(out + pos, outsz - pos, "%s%02x", i ? " " : "", (unsigned)p[i]);
    if (shown < n && pos + 5 < outsz)
        snprintf(out + pos, outsz - pos, " ...");
}

static void print_touch(const uint8_t *p, CFIndex n)
{
    unsigned count, shown;

    g_touch++;
    if (n < TOUCH_PAYLOAD) {
        printf("[%8.3f] report 4  short report (%ld bytes)\n", elapsed(), (long)n);
        return;
    }
    count = p[MAX_CONTACTS * CONTACT_SIZE + 4];
    shown = count > MAX_CONTACTS ? MAX_CONTACTS : count;

    printf("[%8.3f] report 4  count=%u", elapsed(), count);
    for (unsigned i = 0; i < shown; i++) {
        const uint8_t *c = p + i * CONTACT_SIZE;
        unsigned id = c[0] & 0x3F;          /* bits 0-5: contact ID  */
        bool tip = (c[0] & 0x40) != 0;      /* bit 6: tip switch     */
        unsigned x = c[1] | (c[2] << 8);    /* uint16 LE, 0..1920    */
        unsigned y = c[3] | (c[4] << 8);    /* uint16 LE, 0..720     */
        printf("  [id %u %-4s x=%u y=%u]", id, tip ? "down" : "up", x, y);
    }
    putchar('\n');
}

static void print_mouse(const uint8_t *p, CFIndex n)
{
    bool zero = true;
    char hex[96];

    g_mouse++;
    for (CFIndex i = 0; i < n; i++) {
        if (p[i] != 0) {
            zero = false;
            break;
        }
    }
    if (zero) {
        g_mouse_zero++;
        if (g_mouse_zero == 1 || g_mouse_zero % MOUSE_ZERO_EVERY == 0)
            printf("[%8.3f] report 5  mouse emulation (zeros) -> run wisecoco-touch-enable"
                   "  [%lu so far]\n", elapsed(), g_mouse_zero);
        return;
    }
    hex_dump(p, n, hex, sizeof hex);
    printf("[%8.3f] report 5  mouse: %s\n", elapsed(), hex);
}

static void on_report(void *context, IOReturn result, void *sender, IOHIDReportType type,
                      uint32_t reportID, uint8_t *report, CFIndex reportLength)
{
    const uint8_t *p = report;
    CFIndex n = reportLength;
    char hex[96];

    (void)context;
    (void)sender;
    (void)type;

    if (result != kIOReturnSuccess) {
        printf("[%8.3f] input report error 0x%08x\n", elapsed(), (unsigned)result);
        return;
    }

    /* With report IDs the buffer starts with the ID byte (56 bytes for
     * report 4); strip it so offsets match the payload layout. */
    if (reportID == TOUCH_REPORT_ID) {
        if (n >= TOUCH_PAYLOAD + 1 && p[0] == TOUCH_REPORT_ID) {
            p++;
            n--;
        }
        print_touch(p, n);
        return;
    }
    if (n > 0 && reportID != 0 && p[0] == reportID) {
        p++;
        n--;
    }
    if (reportID == MOUSE_REPORT_ID) {
        print_mouse(p, n);
        return;
    }
    g_other++;
    hex_dump(p, n, hex, sizeof hex);
    printf("[%8.3f] report %u  %ld byte(s): %s\n", elapsed(), (unsigned)reportID, (long)n, hex);
}

static void copy_string_prop(IOHIDDeviceRef dev, CFStringRef key, char *out, size_t size)
{
    CFTypeRef v = IOHIDDeviceGetProperty(dev, key);
    if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)v, out, (CFIndex)size, kCFStringEncodingUTF8);
}

static void on_device_matched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    char maker[128] = "?", product[128] = "?";
    uint8_t *buf;

    (void)context;
    (void)result;
    (void)sender;

    copy_string_prop(device, CFSTR(kIOHIDManufacturerKey), maker, sizeof maker);
    copy_string_prop(device, CFSTR(kIOHIDProductKey), product, sizeof product);
    printf("[%8.3f] device: %s %s (%04x:%04x)\n", elapsed(), maker, product, g_vid, g_pid);

    /* One buffer per device, kept for the life of the process. */
    buf = calloc(1, REPORT_BUF_SIZE);
    if (buf == NULL) {
        fprintf(stderr, TOOL_NAME ": out of memory\n");
        return;
    }
    IOHIDDeviceRegisterInputReportCallback(device, buf, REPORT_BUF_SIZE, on_report, NULL);
    g_devices++;
}

static void on_device_removed(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
{
    (void)context;
    (void)result;
    (void)sender;
    (void)device;
    printf("[%8.3f] device removed\n", elapsed());
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: " TOOL_NAME " [--seconds N] [--vid 0xVVVV] [--pid 0xPPPP]\n"
        "\n"
        "Prints the HID input reports of the Wisecoco bar touch controller.\n"
        "Needs Input Monitoring permission for the terminal app.\n"
        "\n"
        "  --seconds N    capture duration (default %d)\n"
        "  --vid 0xVVVV   USB vendor ID, hexadecimal (default 0x%04x)\n"
        "  --pid 0xPPPP   USB product ID, hexadecimal (default 0x%04x)\n"
        "  -h, --help     show this help\n",
        DEFAULT_SECONDS, DEFAULT_VID, DEFAULT_PID);
}

static bool parse_ulong(const char *s, int base, unsigned long max, unsigned long *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return false;
    errno = 0;
    v = strtoul(s, &end, base);
    if (errno != 0 || end == s || *end != '\0' || v > max)
        return false;
    *out = v;
    return true;
}

static CFNumberRef cf_int(int v)
{
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
}

int main(int argc, char **argv)
{
    unsigned long seconds = DEFAULT_SECONDS, v;
    CFMutableDictionaryRef match;
    CFNumberRef nv, np;
    IOHIDManagerRef mgr;
    IOReturn r;
    CFAbsoluteTime end;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--seconds") == 0) {
            if (i + 1 >= argc || !parse_ulong(argv[i + 1], 10, 86400, &v) || v == 0) {
                fprintf(stderr, TOOL_NAME ": --seconds needs a number between 1 and 86400\n");
                return 2;
            }
            seconds = v;
            i++;
        } else if (strcmp(a, "--vid") == 0 || strcmp(a, "--pid") == 0) {
            if (i + 1 >= argc || !parse_ulong(argv[i + 1], 16, 0xFFFF, &v)) {
                fprintf(stderr, TOOL_NAME ": %s needs a hexadecimal ID such as 0x0712\n", a);
                return 2;
            }
            if (a[2] == 'v')
                g_vid = (unsigned)v;
            else
                g_pid = (unsigned)v;
            i++;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, TOOL_NAME ": unknown option '%s'\n\n", a);
            usage(stderr);
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    g_t0 = CFAbsoluteTimeGetCurrent();

    mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    nv = cf_int((int)g_vid);
    np = cf_int((int)g_pid);
    if (mgr == NULL || match == NULL || nv == NULL || np == NULL) {
        fprintf(stderr, TOOL_NAME ": cannot create the HID manager\n");
        return 2;
    }
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), nv);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), np);
    CFRelease(nv);
    CFRelease(np);
    IOHIDManagerSetDeviceMatching(mgr, match);
    CFRelease(match);

    IOHIDManagerRegisterDeviceMatchingCallback(mgr, on_device_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr, on_device_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

    r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    if (r == kIOReturnNotPermitted) {
        fprintf(stderr,
            TOOL_NAME ": IOHIDManagerOpen: not permitted (0xe00002e2).\n"
            "Grant Input Monitoring to the app running this tool (Terminal, iTerm, ...):\n"
            "System Settings > Privacy & Security > Input Monitoring, then quit and\n"
            "reopen that app and run this tool again.\n");
        CFRelease(mgr);
        return 2;
    }
    if (r != kIOReturnSuccess)
        fprintf(stderr, TOOL_NAME ": IOHIDManagerOpen returned 0x%08x; continuing\n", (unsigned)r);

    printf("listening for %04x:%04x for %lu s; touch the screen now\n", g_vid, g_pid, seconds);

    end = g_t0 + (CFAbsoluteTime)seconds;
    for (;;) {
        CFTimeInterval left = end - CFAbsoluteTimeGetCurrent();
        if (left <= 0)
            break;
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, left, false) == kCFRunLoopRunFinished) {
            sleep((unsigned)left + 1); /* nothing scheduled: just wait */
            break;
        }
    }

    printf("\nsummary: %lu device(s), %lu touch report(s) (ID 4), %lu mouse report(s) (ID 5,"
           " %lu all-zero), %lu other\n", g_devices, g_touch, g_mouse, g_mouse_zero, g_other);
    if (g_devices == 0)
        printf("-> no %04x:%04x HID device found: check the TYPE-C cable and"
               " `ioreg -p IOUSB -w0 | grep -i wcidtest`\n", g_vid, g_pid);
    else if (g_touch > 0)
        printf("-> multitouch reports are flowing\n");
    else if (g_mouse_zero > 0)
        printf("-> mouse-emulation mode: run wisecoco-touch-enable\n");
    else
        printf("-> no reports: touch the screen during the capture; if it stays silent,\n"
               "   unplug and replug the bar (power cycle), then run wisecoco-touch-enable\n");

    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return g_devices == 0 ? 1 : 0;
}
