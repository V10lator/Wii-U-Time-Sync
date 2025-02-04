#include "ConfigItemNtpServer.h"
#include "kbd.h"
#include <coreinit/memdefaultheap.h>
#include <string.h>
#include <wups.h>

int32_t WUPSConfigItemNtpServer_getCurrentValueDisplay(void *context, char *out_buf, int32_t out_size) {
    ConfigItemNtpServer *item = (ConfigItemNtpServer *) context;
    strncpy(out_buf, item->value, out_size - 2);
    out_buf[--out_size] = '\0';
    return 0;
}

bool WUPSConfigItemNtpServer_callCallback(void *context) {
    ConfigItemNtpServer *item = (ConfigItemNtpServer *) context;
    if(item->callback != NULL)
    {
        item->callback();
        return true;
    }

    return false;
}


void WUPSConfigItemNtpServer_onButtonPressed(void *context, WUPSConfigButtons buttons) {
    ConfigItemNtpServer *item = (ConfigItemNtpServer *) context;
    if(buttons & WUPS_CONFIG_BUTTON_A)
        renderKeyboard(item->value);
}

bool WUPSConfigItemNtpServer_isMovementAllowed(void *context) {
    (void)context;
    return true;
}

void WUPSConfigItemNtpServer_restoreDefault(void *context) {
    ConfigItemNtpServer *item  = (ConfigItemNtpServer *) context;
    strcpy(item->value, "pool.ntp.org");
}

void WUPSConfigItemNtpServer_onDelete(void *context) {
    MEMFreeToDefaultHeap(context);
}

void WUPSConfigItemNtpServer_onSelected(void *context, bool isSelected) {
    (void)context;
    (void)isSelected;
}

bool WUPSConfigItemNtpServer_AddToCategory(WUPSConfigCategoryHandle cat, const char *configId, const char *displayName, char *value, NtpServerValueChangedCallback callback) {
    if (cat == 0)
        return false;

    ConfigItemNtpServer *item = (ConfigItemNtpServer *) MEMAllocFromDefaultHeap(sizeof(ConfigItemNtpServer));
    if (item == NULL)
        return false;

    item->value = value;
    item->callback = callback;

    WUPSConfigCallbacks_t callbacks = {
            .getCurrentValueDisplay         = &WUPSConfigItemNtpServer_getCurrentValueDisplay,
            .getCurrentValueSelectedDisplay = &WUPSConfigItemNtpServer_getCurrentValueDisplay,
            .onSelected                     = &WUPSConfigItemNtpServer_onSelected,
            .restoreDefault                 = &WUPSConfigItemNtpServer_restoreDefault,
            .isMovementAllowed              = &WUPSConfigItemNtpServer_isMovementAllowed,
            .callCallback                   = &WUPSConfigItemNtpServer_callCallback,
            .onButtonPressed                = &WUPSConfigItemNtpServer_onButtonPressed,
            .onDelete                       = &WUPSConfigItemNtpServer_onDelete};

    if (WUPSConfigItem_Create(&(item->handle), configId, displayName, callbacks, item) < 0) {
        MEMFreeToDefaultHeap(item);
        return false;
    };

    if (WUPSConfigCategory_AddItem(cat, item->handle) < 0) {
        WUPSConfigItem_Destroy(item->handle);
        return false;
    }
    return true;
}
