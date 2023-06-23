#include <arpa/inet.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <coreinit/debug.h>
#include <coreinit/filesystem.h>
#include <coreinit/ios.h>
#include <coreinit/mcp.h>
#include <coreinit/time.h>
#include <nn/pdm.h>
#include <notifications/notifications.h>
#include <whb/proc.h>
#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>

#include "ConfigItemTime.h"
#include <timezone.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <thread>

#define SYNCING_ENABLED_CONFIG_ID "enabledSync"
#define DST_ENABLED_CONFIG_ID "enabledDST"
#define NOTIFY_ENABLED_CONFIG_ID "enabledNotify"
// Seconds between 1900 (NTP epoch) and 2000 (Wii U epoch)
#define NTP_TIMESTAMP_DELTA 3155673600llu

#define LI_UNSYNC 0xc0
#define NTP_SERVER "fritz.box"
#define MODE_MASK 0x07
#define MODE_CLIENT 0x03
#define MODE_SERVER 0x04

// Important plugin information.
WUPS_PLUGIN_NAME("Wii U Time Sync");
WUPS_PLUGIN_DESCRIPTION("A plugin that synchronizes a Wii U's clock to the Internet.");
WUPS_PLUGIN_VERSION("v1.1.0");
WUPS_PLUGIN_AUTHOR("Nightkingale");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("Wii U Time Sync");

static bool enabledSync = false;
static bool enabledDST = false;
static bool enabledNotify = true;

static ConfigItemTime *sysTimeHandle;
static ConfigItemTime *ntpTimeHandle;
static std::thread *settingsThread;
static volatile bool settingsThreadActive;
static int32_t timezoneOffset;

// From https://github.com/lettier/ntpclient/blob/master/source/c/main.c
typedef struct
{
    uint8_t li_vn_mode;      // Eight bits. li, vn, and mode.
                                // li.   Two bits.   Leap indicator.
                                // vn.   Three bits. Version number of the protocol.
                                // mode. Three bits. Client will pick mode 3 for client.

    uint8_t stratum;         // Eight bits. Stratum level of the local clock.
    uint8_t poll;            // Eight bits. Maximum interval between successive messages.
    uint8_t precision;       // Eight bits. Precision of the local clock.

    uint32_t rootDelay;      // 32 bits. Total round trip delay time.
    uint32_t rootDispersion; // 32 bits. Max error aloud from primary clock source.
    uint32_t refId;          // 32 bits. Reference clock identifier.

    uint32_t refTm_s;        // 32 bits. Reference time-stamp seconds.
    uint32_t refTm_f;        // 32 bits. Reference time-stamp fraction of a second.

    uint32_t origTm_s;       // 32 bits. Originate time-stamp seconds.
    uint32_t origTm_f;       // 32 bits. Originate time-stamp fraction of a second.

    uint32_t rxTm_s;         // 32 bits. Received time-stamp seconds.
    uint32_t rxTm_f;         // 32 bits. Received time-stamp fraction of a second.

    uint32_t txTm_s;         // 32 bits and the most important field the client cares about. Transmit time-stamp seconds.
    uint32_t txTm_f;         // 32 bits. Transmit time-stamp fraction of a second.

} ntp_packet;              // Total: 384 bits or 48 bytes.

extern "C" int32_t CCRSysSetSystemTime(OSTime time);
extern "C" BOOL __OSSetAbsoluteSystemTime(OSTime time);

static bool SetSystemTime(OSTime time)
{
    nn::pdm::NotifySetTimeBeginEvent();

    if (CCRSysSetSystemTime(time) != 0) {
        nn::pdm::NotifySetTimeEndEvent();
        return false;
    }

    BOOL res = __OSSetAbsoluteSystemTime(time);

    nn::pdm::NotifySetTimeEndEvent();

    return res != FALSE;
}

static OSTime NTPGetTime(const char* hostname)
{
    OSTime tick = 0;

    // Get host address by name
    struct hostent* server = gethostbyname(hostname);
    if (server) {
        // Create a socket
        int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sockfd > -1) {
            // Prepare socket address
            struct sockaddr_in serv_addr;
            memset(&serv_addr, 0, sizeof(serv_addr));
            serv_addr.sin_family = AF_INET;

            // Copy the server's IP address to the server address structure.
            memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);

            // Convert the port number integer to network big-endian style and save it to the server address structure.
            serv_addr.sin_port = htons(123); // UDP port

            // Call up the server using its IP address and port number.
            if (connect(sockfd, (struct sockaddr*) &serv_addr, sizeof(serv_addr)) == 0) {
                ntp_packet packet __attribute__((__aligned__(0x40)));
                memset(&packet, 0, sizeof(packet));

                // Set the first byte's bits to 00,001,011 for li = 0, vn = 1, and mode = 3. The rest will be left set to zero.
                packet.li_vn_mode = (1 << 3) | MODE_CLIENT;

                // Send it the NTP packet it wants. If n == -1, it failed.
                if (write(sockfd, &packet, sizeof(packet)) == sizeof(packet)) {
                    // Wait and receive the packet back from the server. If n == -1, it failed.
                    if (read(sockfd, &packet, sizeof(packet)) == sizeof(packet)) {
                        // Basic validity check:
                        // li != 11
                        // stratum != 0
                        // transmit timestamp != 0
                        if ((packet.li_vn_mode & LI_UNSYNC) != LI_UNSYNC && (packet.li_vn_mode & MODE_MASK) == MODE_SERVER && packet.stratum != 0 && (packet.txTm_s | packet.txTm_f)) {
                            // These two fields contain the time-stamp seconds as the packet left the NTP server.
                            // The number of seconds correspond to the seconds passed since 1900.
                            // ntohl() converts the bit/byte order from the network's to host's "endianness".
                            packet.txTm_s = ntohl(packet.txTm_s); // Time-stamp seconds.
                            packet.txTm_f = ntohl(packet.txTm_f); // Time-stamp fraction of a second.

                            // Convert timezone
                            packet.txTm_s += timezoneOffset;

                            // Convert seconds to ticks and adjust timestamp
                            tick += OSSecondsToTicks(packet.txTm_s - NTP_TIMESTAMP_DELTA);
                            // Convert fraction to ticks
                            tick += OSNanosecondsToTicks((packet.txTm_f * 1000000000llu) >> 32);
                        }
                    }
                }

                close(sockfd);
            }
        }
    }

    return tick;
}

static void updateTime() {
    OSTime time = NTPGetTime(NTP_SERVER); // Connect to the time server.
    OSTime currentTime = OSGetTime();
    int timeDifference = abs(time - currentTime);

    if (static_cast<uint64_t>(timeDifference) <= OSMillisecondsToTicks(250)) {
        return; // Time difference is within 250 milliseconds, no need to update.
    }

    SetSystemTime(time); // This finally sets the console time.

    if (enabledNotify) {
        NotificationModule_AddInfoNotification("The time has been changed based on your Internet connection.");
    }
}

INITIALIZE_PLUGIN() {
    WUPSStorageError storageRes = WUPS_OpenStorage();
    // Check if the plugin's settings have been saved before.
    if (storageRes == WUPS_STORAGE_ERROR_SUCCESS) {
        if ((storageRes = WUPS_GetBool(nullptr, SYNCING_ENABLED_CONFIG_ID, &enabledSync)) == WUPS_STORAGE_ERROR_NOT_FOUND) {
            WUPS_StoreBool(nullptr, SYNCING_ENABLED_CONFIG_ID, enabledSync);
        }

        if ((storageRes = WUPS_GetBool(nullptr, DST_ENABLED_CONFIG_ID, &enabledDST)) == WUPS_STORAGE_ERROR_NOT_FOUND) {
            WUPS_StoreBool(nullptr, DST_ENABLED_CONFIG_ID, enabledDST);
        }

        if ((storageRes = WUPS_GetBool(nullptr, NOTIFY_ENABLED_CONFIG_ID, &enabledNotify)) == WUPS_STORAGE_ERROR_NOT_FOUND) {
            WUPS_StoreBool(nullptr, NOTIFY_ENABLED_CONFIG_ID, enabledNotify);
        }

        NotificationModule_InitLibrary(); // Set up for notifications.
        WUPS_CloseStorage(); // Close the storage.
    }

    // Set timezone
    setenv("TZ", timezones[321].posix, 1);
    tzset();
    timezoneOffset = -_timezone;
    if (_daylight) {
        timezoneOffset += 3600;
    }

    if (enabledSync) {
        updateTime(); // Update time when plugin is loaded.
    }
}

static void syncingEnabled(ConfigItemBoolean *item, bool value)
{
    // If false, bro is literally a time traveler!
    WUPS_StoreBool(nullptr, SYNCING_ENABLED_CONFIG_ID, value);
    enabledSync = value;
}

static void savingsEnabled(ConfigItemBoolean *item, bool value)
{
    WUPS_StoreBool(nullptr, DST_ENABLED_CONFIG_ID, value);
    enabledDST = value;
}

static void notifyEnabled(ConfigItemBoolean *item, bool value)
{
    WUPS_StoreBool(nullptr, NOTIFY_ENABLED_CONFIG_ID, value);
    enabledNotify = value;
}

static void setTimeInSettings() {
    OSCalendarTime ct;
    OSTime ot;
    char timeString[256];

    ot = NTPGetTime(NTP_SERVER);
    if (ot == 0) {
        WUPSConfigItem_SetDisplayName(ntpTimeHandle->handle, "Current NTP Time: N/A");
    } else {
        OSTicksToCalendarTime(ot, &ct);
        snprintf(timeString, 255, "Current NTP Time: %04d-%02d-%02d %02d:%02d:%02d:%04d:%04d\n", ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec, ct.tm_msec, ct.tm_usec);
        WUPSConfigItem_SetDisplayName(ntpTimeHandle->handle, timeString);
    }

    ot = OSGetTime();
    OSTicksToCalendarTime(ot, &ct);
    snprintf(timeString, 255, "Current SYS Time: %04d-%02d-%02d %02d:%02d:%02d:%04d:%04d\n", ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec, ct.tm_msec, ct.tm_usec);
    WUPSConfigItem_SetDisplayName(sysTimeHandle->handle, timeString);
}

static void settingsThreadMain() {
    while(settingsThreadActive) {
        setTimeInSettings();
        sleep(1);
    }
}

WUPS_GET_CONFIG() {
    if (WUPS_OpenStorage() != WUPS_STORAGE_ERROR_SUCCESS) {
        return 0;
    }

    WUPSConfigHandle settings;
    WUPSConfig_CreateHandled(&settings, "Wii U Time Sync");

    WUPSConfigCategoryHandle config;
    WUPSConfig_AddCategoryByNameHandled(settings, "Configuration", &config);
    WUPSConfigCategoryHandle preview;
    WUPSConfig_AddCategoryByNameHandled(settings, "Preview Time", &preview);

    WUPSConfigItemBoolean_AddToCategoryHandled(settings, config, "enabledSync", "Syncing Enabled", enabledSync, &syncingEnabled);
    WUPSConfigItemBoolean_AddToCategoryHandled(settings, config, "enabledDST", "Daylight Savings", enabledDST, &savingsEnabled);
    WUPSConfigItemBoolean_AddToCategoryHandled(settings, config, "enabledNotify", "Receive Notifications", enabledNotify, &notifyEnabled);

    sysTimeHandle = WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "sysTime", "Current SYS Time: Loading...");
    ntpTimeHandle = WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "ntpTime", "Current NTP Time: Loading...");
    settingsThreadActive = true;
    settingsThread = new std::thread(settingsThreadMain);

    char timeString[256];
    snprintf(timeString, 255, "Timezone: %s%dsecs (%s / %s)", timezoneOffset < 0 ? "" : "+", timezoneOffset, _tzname[0], _tzname[1]);
    WUPSConfigItemTime_AddToCategoryHandled(settings, preview, "TZ", timeString);

    return settings;
}

WUPS_CONFIG_CLOSED() {
    if (enabledSync) {
        std::thread updateTimeThread(updateTime);
        updateTimeThread.detach(); // Update time when settings are closed.
    }
    
    settingsThreadActive = false;
    settingsThread->join();
    delete settingsThread;
    WUPS_CloseStorage(); // Save all changes.
}
