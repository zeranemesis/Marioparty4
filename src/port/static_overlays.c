#include "port/static_overlays.h"

#include <stdlib.h>
#include <string.h>

PartyBoardStaticOverlay *PartyBoard_StaticOverlayLink(const char *name)
{
    unsigned i;
    for (i = 0; i < PartyBoard_StaticOverlayCount; ++i) {
        PartyBoardStaticOverlay *overlay = &PartyBoard_StaticOverlays[i];
        size_t dataSize;
        if (strcmp(overlay->name, name) != 0) {
            continue;
        }
        dataSize = (size_t)(overlay->dataEnd - overlay->dataStart);
        if (dataSize != 0) {
            /* Nothing runs an overlay's code before its first link, so its data
               is still the initial image then: keep a copy to restore later. */
            if (overlay->pristine == NULL) {
                overlay->pristine = (unsigned char *)malloc(dataSize);
                if (overlay->pristine != NULL) {
                    memcpy(overlay->pristine, overlay->dataStart, dataSize);
                }
            } else {
                memcpy(overlay->dataStart, overlay->pristine, dataSize);
            }
        }
        if (overlay->bssEnd != overlay->bssStart) {
            memset(overlay->bssStart, 0, (size_t)(overlay->bssEnd - overlay->bssStart));
        }
        return overlay;
    }
    return NULL;
}
