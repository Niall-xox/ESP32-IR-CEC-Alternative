// HID Two-Way Communication Test — PC Side
//
// Finds the ESP32 HID test device by VID/PID, sends a test payload,
// and prints the response. Confirms that two-way HID communication works
// before implementing it in the main firmware and daemon.

#include <hidapi/hidapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Must match TEST_VID and TEST_PID in the ESP32 firmware
#define TARGET_VID  0x1234
#define TARGET_PID  0x5678

// Must match REPORT_SIZE in the ESP32 firmware
#define REPORT_SIZE 64

int main() {
    hid_init();

    // Enumerate all connected HID devices so the user can see what's available.
    // Useful if the device isn't found — the actual VID/PID will be listed here.
    printf("=== Connected HID devices ===\n");
    hid_device_info* devs = hid_enumerate(0, 0);
    for (auto d = devs; d; d = d->next) {
        printf("  VID=%04X PID=%04X  %ls\n", d->vendor_id, d->product_id,
               d->product_string ? d->product_string : L"(no name)");
    }
    hid_free_enumeration(devs);
    printf("\n");

    // Open the ESP32 HID device
    hid_device* dev = hid_open(TARGET_VID, TARGET_PID, nullptr);
    if (!dev) {
        printf("[error] Device not found (VID=%04X PID=%04X)\n", TARGET_VID, TARGET_PID);
        printf("        Is the ESP32 plugged in and the HID firmware flashed?\n");
        hid_exit();
        return 1;
    }
    printf("[ok] Device opened (VID=%04X PID=%04X)\n\n", TARGET_VID, TARGET_PID);

    // Build the output report.
    // hidapi requires the first byte to be the report ID (0 = no report ID).
    uint8_t txBuf[REPORT_SIZE + 1] = {0};
    txBuf[0] = 0x00;  // report ID
    const char* msg = "HELLO FROM PC";
    memcpy(txBuf + 1, msg, strlen(msg));

    printf("[tx] Sending: \"%s\"\n", msg);
    int res = hid_write(dev, txBuf, sizeof(txBuf));
    if (res < 0) {
        printf("[error] Write failed: %ls\n", hid_error(dev));
        hid_close(dev);
        hid_exit();
        return 1;
    }

    // Wait up to 2 seconds for the ESP32 to echo back
    uint8_t rxBuf[REPORT_SIZE] = {0};
    res = hid_read_timeout(dev, rxBuf, sizeof(rxBuf), 2000);

    if (res < 0) {
        printf("[error] Read failed: %ls\n", hid_error(dev));
    } else if (res == 0) {
        printf("[error] Timeout — no response received\n");
    } else {
        // Response should be "ACK:HELLO FROM PC"
        printf("[rx] Received: \"%s\"\n", reinterpret_cast<char*>(rxBuf));
        printf("\n[ok] Two-way HID communication working\n");
    }

    hid_close(dev);
    hid_exit();
    return 0;
}
