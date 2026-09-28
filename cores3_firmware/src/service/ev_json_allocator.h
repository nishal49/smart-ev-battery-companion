#ifndef EV_JSON_ALLOCATOR_H
#define EV_JSON_ALLOCATOR_H

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

/* Parse API responses out of PSRAM so JSON buffers never compete with the
   Wi-Fi stack and TLS handshake for scarce internal RAM. Falls back to the
   default heap if PSRAM is unavailable. */
struct EvPsramAllocator : ArduinoJson::Allocator {
    void * allocate(size_t size) override
    {
        void * pointer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        return pointer != nullptr ? pointer : malloc(size);
    }

    void deallocate(void * pointer) override
    {
        heap_caps_free(pointer);
    }

    void * reallocate(void * pointer, size_t size) override
    {
        void * moved = heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM);
        return moved != nullptr ? moved : realloc(pointer, size);
    }
};

#endif /* EV_JSON_ALLOCATOR_H */
