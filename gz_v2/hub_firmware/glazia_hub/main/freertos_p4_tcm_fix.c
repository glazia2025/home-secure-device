/* IDF 5.3 bug on ESP32-P4: TCM (0x30100000-0x30102000) is added to the heap
 * with MALLOC_CAP_INTERNAL, so pvPortMalloc can return TCM addresses.
 * But xPortcheckValidStackMem / xPortCheckValidTCBMem use esp_ptr_internal()
 * which only covers the DRAM range (0x4ff00000-0x4ffc0000) and rejects TCM,
 * causing a configASSERT inside xTaskCreateStaticPinnedToCore at boot.
 *
 * Fix: redirect both checks via --wrap to also accept TCM addresses.
 * TCM is fast internal SRAM and is perfectly valid for stack/TCB storage.
 */
#include <stdbool.h>
#include "esp_memory_utils.h"

bool __wrap_xPortcheckValidStackMem(const void *ptr)
{
    /* Accept TCM on every config path. With CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
     * enabled the wrapper below returns early on esp_ptr_byte_accessible(), which does
     * NOT whitelist TCM, so the TCM check must come first. TCM is fast internal SRAM
     * and is perfectly valid for stack storage. */
#if SOC_MEM_TCM_SUPPORTED
    if (esp_ptr_in_tcm(ptr)) return true;
#endif
#ifdef CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    return esp_ptr_byte_accessible(ptr);
#else
    return esp_ptr_internal(ptr) && esp_ptr_byte_accessible(ptr);
#endif
}

bool __wrap_xPortCheckValidTCBMem(const void *ptr)
{
    if (esp_ptr_internal(ptr) && esp_ptr_byte_accessible(ptr)) return true;
#if SOC_MEM_TCM_SUPPORTED
    if (esp_ptr_in_tcm(ptr)) return true;
#endif
    return false;
}
