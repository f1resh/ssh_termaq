// Serial ports on macOS: enumeration through IOKit (the call-out devices /dev/cu.*, named after their USB product), and
// the parts of the POSIX port code (posix/sys/SerialPosix.cpp) that are macOS' own: any speed (IOSSIOSPEED) and
// DSR/DTR flow control.
#include "platform/sys/Serial.h"

#include "platform/detail/PosixSerial.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/serial/ioss.h>
#include <sys/ioctl.h>
#include <termios.h>

#include <vector>

namespace st::sys::serial {

namespace {

std::string toUtf8(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    const auto s = static_cast<CFStringRef>(value);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(static_cast<size_t>(max));
    return CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8) ? std::string(buf.data()) : std::string();
}

/** A string property of the service itself (`parents` false) or of the nearest service above it that has it. */
std::string property(io_object_t service, const char* key, bool parents) {
    const CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    const CFTypeRef value =
        parents ? IORegistryEntrySearchCFProperty(service, kIOServicePlane, name, kCFAllocatorDefault,
                                                  kIORegistryIterateRecursively | kIORegistryIterateParents)
                : IORegistryEntryCreateCFProperty(service, name, kCFAllocatorDefault, 0);
    CFRelease(name);
    std::string out = toUtf8(value);
    if (value) CFRelease(value);
    return out;
}

}  // namespace

std::vector<PortInfo> listPorts() {
    std::vector<PortInfo> out;
    CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!match) return out;
    CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &it) != KERN_SUCCESS) return out;  // releases `match`
    while (io_object_t service = IOIteratorNext(it)) {
        PortInfo p;
        // The call-out device: opening it does not wait for a carrier (the /dev/tty.* twin does).
        p.path = property(service, kIOCalloutDeviceKey, false);
        p.friendlyName = property(service, "USB Product Name", true);
        p.manufacturer = property(service, "USB Vendor Name", true);
        if (p.friendlyName.empty()) p.friendlyName = property(service, kIOTTYDeviceKey, false);  // "Bluetooth-Incoming-Port"
        if (!p.path.empty()) out.push_back(std::move(p));
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    return out;
}

namespace detail {

bool setCustomSpeed(int fd, int baudRate) {
    speed_t speed = static_cast<speed_t>(baudRate);
    return ::ioctl(fd, IOSSIOSPEED, &speed) == 0;
}

unsigned long dsrDtrFlowFlags() { return CDTR_IFLOW | CDSR_OFLOW; }

}  // namespace detail

}  // namespace st::sys::serial
