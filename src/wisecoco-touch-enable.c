/*
 * wisecoco-touch-enable - switch the Wisecoco 12.3" bar touch controller
 * from its broken macOS mouse emulation to multitouch reports.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 wisecoco-touchscreen-macos contributors
 * See the LICENSE file for the full license text.
 *
 * macOS enables DEVICE_REMOTE_WAKEUP when it configures the touch controller
 * (USB 0712:0010, "hotlotus" / "wcidtest"). The firmware then falls back to
 * a single-touch mouse emulation that only sends all-zero reports. This tool
 * clears remote wakeup and replays the control requests Windows sends after
 * SET_CONFIGURATION; the controller then streams report ID 4 (Windows 8 style
 * multitouch digitizer).
 *
 * Runs unprivileged from user space. Requests go to the default control pipe
 * through IOUSBDeviceInterface::DeviceRequest without opening the device; it
 * falls back to USBDeviceOpen only if the system answers kIOReturnNotOpen.
 *
 * Build:
 *   clang -O2 -Wall -o wisecoco-touch-enable wisecoco-touch-enable.c \
 *         -framework IOKit -framework CoreFoundation
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_error.h>

#define TOOL_NAME        "wisecoco-touch-enable"
#define TOOL_VERSION     "1.0.0"

#define DEFAULT_VID      0x0712
#define DEFAULT_PID      0x0010
#define HID_INTERFACE    0x0001

#define PUBLISH_DELAY_S  0.3      /* wait after a device is published      */
#define WAKE_DELAY_S     2.0      /* wait after the system has powered on  */
#define STEP_US          100000   /* 100 ms between polls / retries        */
#define MAX_ATTEMPTS     10       /* ~1 s of sequence retries              */
#define MAX_CONFIG_POLLS 10       /* ~1 s waiting for interfaces           */
#define MAX_OPEN_TRIES   5

enum seq_result { SEQ_OK, SEQ_RETRY, SEQ_FAIL };

typedef struct {
    IOUSBDeviceInterface **intf;
    UInt32 location;
    bool opened;
    char tag[40];
} usb_dev;

static UInt16 g_vid = DEFAULT_VID;
static UInt16 g_pid = DEFAULT_PID;
static bool g_verbose = false;
static io_connect_t g_root_port = IO_OBJECT_NULL;
static char g_reason[160];

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    printf("%s ", ts);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static const char *err_str(IOReturn kr)
{
    static char buf[96];
    if (kr == kIOReturnSuccess)
        return "ok";
    snprintf(buf, sizeof buf, "0x%08x (%s)", (unsigned)kr, mach_error_string(kr));
    return buf;
}

static void hex_preview(const UInt8 *p, UInt32 n, char *out, size_t outsz)
{
    UInt32 shown = n < 16 ? n : 16;
    size_t pos = 0;

    out[0] = '\0';
    for (UInt32 i = 0; i < shown && pos + 4 < outsz; i++)
        pos += (size_t)snprintf(out + pos, outsz - pos, "%s%02x", i ? " " : "", (unsigned)p[i]);
    if (shown < n && pos + 5 < outsz)
        snprintf(out + pos, outsz - pos, " ...");
}

/* ------------------------------------------------------------------------ */
/* USB access                                                               */
/* ------------------------------------------------------------------------ */

static CFMutableDictionaryRef make_matching(void)
{
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    SInt32 vid = g_vid, pid = g_pid;
    CFNumberRef nv, np;

    if (m == NULL)
        return NULL;
    nv = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vid);
    np = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pid);
    if (nv == NULL || np == NULL) {
        if (nv) CFRelease(nv);
        if (np) CFRelease(np);
        CFRelease(m);
        return NULL;
    }
    CFDictionarySetValue(m, CFSTR("idVendor"), nv);
    CFDictionarySetValue(m, CFSTR("idProduct"), np);
    CFRelease(nv);
    CFRelease(np);
    return m;
}

/* Interfaces are published once SET_CONFIGURATION has completed. This only
 * reads the I/O Registry and sends nothing to the device. */
static bool has_interfaces(io_service_t service)
{
    io_iterator_t it = IO_OBJECT_NULL;
    io_object_t child;
    bool found = false;

    if (IORegistryEntryGetChildIterator(service, kIOServicePlane, &it) != KERN_SUCCESS)
        return true; /* cannot tell; let the request sequence decide */
    while (!found && (child = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        found = IOObjectConformsTo(child, "IOUSBHostInterface") != 0;
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
    return found;
}

static bool open_usb_dev(io_service_t service, usb_dev *d)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = KERN_FAILURE;
    HRESULT hr;

    memset(d, 0, sizeof *d);
    for (int i = 0; i < MAX_OPEN_TRIES; i++) {
        kr = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugin, &score);
        if (kr == KERN_SUCCESS && plugin != NULL)
            break;
        plugin = NULL;
        usleep(2 * STEP_US);
    }
    if (plugin == NULL) {
        say("%04x:%04x: cannot create USB plug-in interface: %s",
            (unsigned)g_vid, (unsigned)g_pid, err_str(kr));
        return false;
    }

    hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),
                                   (LPVOID *)&d->intf);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK || d->intf == NULL) {
        say("%04x:%04x: cannot get IOUSBDeviceInterface (HRESULT 0x%08x)",
            (unsigned)g_vid, (unsigned)g_pid, (unsigned)hr);
        d->intf = NULL;
        return false;
    }

    if ((*d->intf)->GetLocationID(d->intf, &d->location) != kIOReturnSuccess)
        d->location = 0;
    snprintf(d->tag, sizeof d->tag, "%04x:%04x @ 0x%08x",
             (unsigned)g_vid, (unsigned)g_pid, (unsigned)d->location);
    return true;
}

static void close_usb_dev(usb_dev *d)
{
    if (d->intf == NULL)
        return;
    if (d->opened)
        (*d->intf)->USBDeviceClose(d->intf);
    (*d->intf)->Release(d->intf);
    d->intf = NULL;
}

/* One control transfer on the default pipe. */
static IOReturn xfer(usb_dev *d, UInt8 bmRequestType, UInt8 bRequest, UInt16 wValue,
                     UInt16 wIndex, UInt16 wLength, void *data, UInt32 *done,
                     const char *what)
{
    IOUSBDevRequest req;
    IOReturn kr;

    memset(&req, 0, sizeof req);
    req.bmRequestType = bmRequestType;
    req.bRequest = bRequest;
    req.wValue = wValue;
    req.wIndex = wIndex;
    req.wLength = wLength;
    req.pData = data;
    req.wLenDone = 0;

    kr = (*d->intf)->DeviceRequest(d->intf, &req);
    if (kr == kIOReturnNotOpen && !d->opened) {
        IOReturn okr = (*d->intf)->USBDeviceOpen(d->intf);
        if (g_verbose)
            say("  USBDeviceOpen: %s", err_str(okr));
        if (okr == kIOReturnSuccess) {
            d->opened = true;
            req.wLenDone = 0;
            kr = (*d->intf)->DeviceRequest(d->intf, &req);
        }
    }
    if (done)
        *done = req.wLenDone;

    if (g_verbose) {
        char hex[80] = "";
        if ((bmRequestType & 0x80) && kr == kIOReturnSuccess && data && req.wLenDone)
            hex_preview(data, req.wLenDone, hex, sizeof hex);
        say("  bmRequestType=0x%02x bRequest=0x%02x wValue=0x%04x wIndex=0x%04x wLength=%-4u"
            " %-28s -> %s, %u byte(s)%s%s",
            (unsigned)bmRequestType, (unsigned)bRequest, (unsigned)wValue,
            (unsigned)wIndex, (unsigned)wLength, what, err_str(kr),
            (unsigned)req.wLenDone, hex[0] ? ": " : "", hex);
    }
    return kr;
}

/* ------------------------------------------------------------------------ */
/* The enable sequence                                                      */
/* ------------------------------------------------------------------------ */

static enum seq_result transient(IOReturn kr, const char *step)
{
    snprintf(g_reason, sizeof g_reason, "%s failed: %s", step, err_str(kr));
    return kr == kIOReturnNoDevice ? SEQ_FAIL : SEQ_RETRY;
}

/*
 * Order matters: remote wakeup must be cleared before the HID feature
 * reports are read. Reading feature reports 2/3 while the controller is in
 * mouse-emulation mode makes it go silent until it is power-cycled, so the
 * sequence stops before step 6 unless GET_STATUS confirms the clear.
 */
static enum seq_result run_sequence(usb_dev *d, int *warnings, UInt8 status[2])
{
    static UInt8 buf[1024];
    UInt32 n = 0;
    IOReturn kr;

    *warnings = 0;

    /* 1. CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP): the actual fix. */
    kr = xfer(d, 0x00, 0x01, 0x0001, 0x0000, 0, NULL, &n, "CLEAR_FEATURE(REMOTE_WAKEUP)");
    if (kr != kIOReturnSuccess)
        return transient(kr, "CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP)");

    /* 2. MS OS 1.0 extended compat ID descriptor, as Windows reads it. */
    kr = xfer(d, 0xC0, 0x60, 0x0000, 0x0004, 16, buf, &n, "MS OS compat ID (16)");
    if (kr != kIOReturnSuccess)
        (*warnings)++;
    kr = xfer(d, 0xC0, 0x60, 0x0000, 0x0004, 40, buf, &n, "MS OS compat ID (40)");
    if (kr != kIOReturnSuccess)
        (*warnings)++;

    /* 3. GET_STATUS(device): bit 1 (remote wakeup) must now be clear. */
    buf[0] = buf[1] = 0xFF;
    kr = xfer(d, 0x80, 0x00, 0x0000, 0x0000, 2, buf, &n, "GET_STATUS");
    if (kr != kIOReturnSuccess)
        return transient(kr, "GET_STATUS");
    if (n < 2) {
        snprintf(g_reason, sizeof g_reason, "GET_STATUS returned %u byte(s)", (unsigned)n);
        return SEQ_RETRY;
    }
    status[0] = buf[0];
    status[1] = buf[1];
    if (buf[0] & 0x02) {
        snprintf(g_reason, sizeof g_reason,
                 "remote wakeup still enabled (GET_STATUS %02x %02x); "
                 "feature reports not read", (unsigned)buf[0], (unsigned)buf[1]);
        return SEQ_RETRY;
    }

    /* 4. HID SET_IDLE on interface 1 (fails until the device is configured). */
    kr = xfer(d, 0x21, 0x0A, 0x0000, HID_INTERFACE, 0, NULL, &n, "SET_IDLE");
    if (kr != kIOReturnSuccess)
        return transient(kr, "SET_IDLE");

    /* 5. HID report descriptor. */
    kr = xfer(d, 0x81, 0x06, 0x2200, HID_INTERFACE, 904, buf, &n, "GET_DESCRIPTOR(HID report)");
    if (kr != kIOReturnSuccess)
        (*warnings)++;

    /* 6. Feature reports 2 (contact count maximum) and 3 (THQA blob). */
    kr = xfer(d, 0xA1, 0x01, 0x0302, HID_INTERFACE, 257, buf, &n, "GET_REPORT(feature 2)");
    if (kr != kIOReturnSuccess)
        (*warnings)++;
    kr = xfer(d, 0xA1, 0x01, 0x0303, HID_INTERFACE, 257, buf, &n, "GET_REPORT(feature 3)");
    if (kr != kIOReturnSuccess)
        (*warnings)++;

    return SEQ_OK;
}

static bool apply_to_service(io_service_t service)
{
    usb_dev d;
    UInt8 status[2] = { 0xFF, 0xFF };
    enum seq_result rc = SEQ_FAIL;
    int warnings = 0;

    for (int i = 0; i < MAX_CONFIG_POLLS && !has_interfaces(service); i++)
        usleep(STEP_US);

    if (!open_usb_dev(service, &d))
        return false;

    if (g_verbose)
        say("%s: applying enable sequence", d.tag);

    g_reason[0] = '\0';
    for (int attempt = 1; ; attempt++) {
        rc = run_sequence(&d, &warnings, status);
        if (rc != SEQ_RETRY)
            break;
        if (attempt >= MAX_ATTEMPTS) {
            rc = SEQ_FAIL;
            break;
        }
        if (g_verbose)
            say("%s: attempt %d: %s; retrying", d.tag, attempt, g_reason);
        usleep(STEP_US);
    }

    if (rc == SEQ_OK) {
        if (warnings)
            say("%s: multitouch enabled (GET_STATUS %02x %02x), %d optional request(s) failed",
                d.tag, (unsigned)status[0], (unsigned)status[1], warnings);
        else
            say("%s: multitouch enabled (GET_STATUS %02x %02x)",
                d.tag, (unsigned)status[0], (unsigned)status[1]);
    } else {
        say("%s: not enabled: %s", d.tag, g_reason[0] ? g_reason : "unknown error");
    }

    close_usb_dev(&d);
    return rc == SEQ_OK;
}

static kern_return_t apply_all_present(int *found, int *enabled)
{
    io_iterator_t it = IO_OBJECT_NULL;
    io_service_t s;
    CFMutableDictionaryRef m = make_matching();
    kern_return_t kr;

    *found = 0;
    *enabled = 0;
    if (m == NULL)
        return kIOReturnNoMemory;
    kr = IOServiceGetMatchingServices(kIOMainPortDefault, m, &it); /* consumes m */
    if (kr != KERN_SUCCESS)
        return kr;
    while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        (*found)++;
        if (apply_to_service(s))
            (*enabled)++;
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return KERN_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Daemon mode                                                              */
/* ------------------------------------------------------------------------ */

typedef void (*deferred_fn)(void *arg);
typedef struct {
    deferred_fn fn;
    void *arg;
} deferred_call;

static void deferred_fire(CFRunLoopTimerRef timer, void *info)
{
    deferred_call *c = info;
    (void)timer;
    c->fn(c->arg);
    free(c); /* one-shot timer: never fires again */
}

static void run_after(double seconds, deferred_fn fn, void *arg)
{
    deferred_call *c = malloc(sizeof *c);
    CFRunLoopTimerContext ctx = { 0, NULL, NULL, NULL, NULL };
    CFRunLoopTimerRef t;

    if (c == NULL) {
        fn(arg);
        return;
    }
    c->fn = fn;
    c->arg = arg;
    ctx.info = c;
    t = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + seconds,
                             0, 0, 0, deferred_fire, &ctx);
    if (t == NULL) {
        free(c);
        fn(arg);
        return;
    }
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRelease(t); /* the run loop keeps it alive until it fires */
}

static void deferred_apply(void *arg)
{
    io_service_t s = (io_service_t)(uintptr_t)arg;
    apply_to_service(s);
    IOObjectRelease(s);
}

static void reapply_all(void *arg)
{
    int found = 0, enabled = 0;
    kern_return_t kr;

    (void)arg;
    kr = apply_all_present(&found, &enabled);
    if (kr != KERN_SUCCESS)
        say("after wake: device lookup failed: %s", err_str(kr));
    else
        say("after wake: %d device(s) found, %d enabled", found, enabled);
}

/* refcon is non-NULL only for the initial scan done from main(). */
static void on_device_published(void *refcon, io_iterator_t it)
{
    io_service_t s;

    while ((s = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        say("%04x:%04x %s", (unsigned)g_vid, (unsigned)g_pid,
            refcon ? "already connected" : "attached");
        /* Ownership of s passes to deferred_apply(). */
        run_after(PUBLISH_DELAY_S, deferred_apply, (void *)(uintptr_t)s);
    }
}

static void on_power(void *refcon, io_service_t service, uint32_t type, void *arg)
{
    (void)refcon;
    (void)service;

    switch (type) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(g_root_port, (intptr_t)arg);
        break;
    case kIOMessageSystemWillSleep:
        if (g_verbose)
            say("system is going to sleep");
        IOAllowPowerChange(g_root_port, (intptr_t)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        say("system woke up; re-applying in %.0f s", WAKE_DELAY_S);
        run_after(WAKE_DELAY_S, reapply_all, NULL);
        break;
    default:
        break;
    }
}

static int run_daemon(void)
{
    IONotificationPortRef usb_port;
    IONotificationPortRef pm_port = NULL;
    io_object_t pm_notifier = IO_OBJECT_NULL;
    io_iterator_t added = IO_OBJECT_NULL;
    CFMutableDictionaryRef m;
    kern_return_t kr;

    usb_port = IONotificationPortCreate(kIOMainPortDefault);
    if (usb_port == NULL) {
        say("IONotificationPortCreate failed");
        return 2;
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(usb_port),
                       kCFRunLoopDefaultMode);

    m = make_matching();
    if (m == NULL) {
        say("cannot build the matching dictionary");
        return 2;
    }
    kr = IOServiceAddMatchingNotification(usb_port, kIOFirstPublishNotification, m,
                                          on_device_published, NULL, &added); /* consumes m */
    if (kr != KERN_SUCCESS) {
        say("IOServiceAddMatchingNotification failed: %s", err_str(kr));
        return 2;
    }

    g_root_port = IORegisterForSystemPower(NULL, &pm_port, on_power, &pm_notifier);
    if (g_root_port == IO_OBJECT_NULL || pm_port == NULL)
        say("warning: IORegisterForSystemPower failed; wake from sleep is not handled");
    else
        CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(pm_port),
                           kCFRunLoopDefaultMode);

    say(TOOL_NAME " " TOOL_VERSION ": watching for %04x:%04x (plug-in and wake from sleep)",
        (unsigned)g_vid, (unsigned)g_pid);

    /* Draining the iterator handles devices that are already connected and
     * arms the notification for future ones. */
    on_device_published((void *)1, added);

    CFRunLoopRun();
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Command line                                                             */
/* ------------------------------------------------------------------------ */

static void usage(FILE *f)
{
    fprintf(f,
        "usage: " TOOL_NAME " [--once] [-v] [--vid 0xVVVV] [--pid 0xPPPP]\n"
        "\n"
        "Switches the Wisecoco bar touch controller (USB 0712:0010) to multitouch\n"
        "by clearing USB remote wakeup and replaying the Windows init requests.\n"
        "\n"
        "  (no option)    keep running and re-apply on plug-in and wake from sleep\n"
        "  --once         apply to connected devices and exit\n"
        "                 exit status: 0 enabled, 1 no device found, 2 error\n"
        "  -v, --verbose  log every control request\n"
        "  --vid 0xVVVV   USB vendor ID, hexadecimal (default 0x%04x)\n"
        "  --pid 0xPPPP   USB product ID, hexadecimal (default 0x%04x)\n"
        "  -h, --help     show this help\n"
        "  --version      show the version\n",
        DEFAULT_VID, DEFAULT_PID);
}

static bool parse_hex16(const char *s, UInt16 *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || *s == '\0')
        return false;
    errno = 0;
    v = strtoul(s, &end, 16); /* accepts an optional 0x prefix */
    if (errno != 0 || end == s || *end != '\0' || v > 0xFFFF)
        return false;
    *out = (UInt16)v;
    return true;
}

int main(int argc, char **argv)
{
    bool once = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--once") == 0) {
            once = true;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            g_verbose = true;
        } else if (strcmp(a, "--vid") == 0 || strcmp(a, "--pid") == 0) {
            UInt16 *dst = (a[2] == 'v') ? &g_vid : &g_pid;
            if (i + 1 >= argc || !parse_hex16(argv[i + 1], dst)) {
                fprintf(stderr, TOOL_NAME ": %s needs a hexadecimal ID such as 0x0712\n", a);
                return 2;
            }
            i++;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf(TOOL_NAME " " TOOL_VERSION "\n");
            return 0;
        } else {
            fprintf(stderr, TOOL_NAME ": unknown option '%s'\n\n", a);
            usage(stderr);
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (once) {
        int found = 0, enabled = 0;
        kern_return_t kr = apply_all_present(&found, &enabled);
        if (kr != KERN_SUCCESS) {
            say("device lookup failed: %s", err_str(kr));
            return 2;
        }
        if (found == 0) {
            say("no %04x:%04x device found (check the TYPE-C cable)",
                (unsigned)g_vid, (unsigned)g_pid);
            return 1;
        }
        say("%d of %d device(s) enabled", enabled, found);
        return enabled > 0 ? 0 : 2;
    }

    return run_daemon();
}
