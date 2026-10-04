#pragma once

#include "config.h"
#include "engine_compat.h"

#ifdef HAVE_LIB_XML2
#include <libxml/parser.h>
#define ENGINE_XPATH_AVAILABLE 1
static inline int engine_xpath_init(void)
{
    static int configured;
    if (!configured) {
        xmlInitParser();
        configured = 1;
    }
    return 0;
}
static inline void *engine_xpath_alloc(size_t size) { return malloc(size); }
static inline void engine_xpath_free(void *ptr) { free(ptr); }
#endif
