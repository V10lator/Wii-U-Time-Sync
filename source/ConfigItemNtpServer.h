#include <wups.h>

#define MAX_NTP_SERVER_LENTGH 32

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ConfigItemNtpServer {
    WUPSConfigItemHandle handle;
    char defaultValue[MAX_NTP_SERVER_LENTGH];
    char value[MAX_NTP_SERVER_LENTGH];
    void *callback;
} ConfigItemNtpServer;

typedef void (*NtpServerValueChangedCallback)(ConfigItemNtpServer *, const char *);

bool WUPSConfigItemNtpServer_AddToCategory(WUPSConfigCategoryHandle cat, const char *configId, const char *displayName,
                                              const char *defaultValue, NtpServerValueChangedCallback callback);

#define WUPSConfigItemNtpServer_AddToCategoryHandled(__config__, __cat__, __configId__, __displayName__, __defaultValue__, __callback__)    \
    do {                                                                                                                                    \
        if (!WUPSConfigItemNtpServer_AddToCategory(__cat__, __configId__, __displayName__, __defaultValue__, __callback__)) {               \
            WUPSConfig_Destroy(__config__);                                                                                                 \
            return 0;                                                                                                                       \
        }                                                                                                                                   \
    } while (0)

#ifdef __cplusplus
}
#endif
