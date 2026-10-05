#include "engine.h"
#include "engine_fs.h"
#include "engine_utf8.h"
#include "xpath_compat.h"

#ifdef ENGINE_XPATH_AVAILABLE

#include <libxml/HTMLparser.h>
#include <libxml/xmlmemory.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum
{
    XPATH_ENGINE_ERR_OPEN = -2,
    XPATH_ENGINE_ERR_PARSE = -3,
    XPATH_ENGINE_ERR_XPATH = -4,
    XPATH_ENGINE_ERR_NON_SCALAR = -5,
};

static void xpath_register_namespaces(xmlXPathContextPtr ctx, xmlDocPtr doc)
{
    (void)doc;
    (void)xmlXPathRegisterNs(ctx, BAD_CAST "html", BAD_CAST "http://www.w3.org/1999/xhtml");
}

static int append_node_string(xmlNodePtr node, char *val_buf, size_t val_sz)
{
    xmlChar *s = xmlNodeGetContent(node);
    if (!s)
    {
        val_buf[0] = '\0';
        return 0;
    }
    snprintf(val_buf, val_sz, "%s", (const char *)s);
    xmlFree(s);
    engine_utf8_fold(val_buf, val_sz);
    return 0;
}

static int xpath_write_nodeset(xmlNodeSetPtr ns, struct xfs_engine_mount_t *ram, struct xfs_handle_t *out_h, char *val_buf, size_t val_sz)
{
    const int nr = ns ? ns->nodeNr : 0;
    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d", nr);
    if (engine_fs_write_le_string(ram, out_h, count_buf) != 0)
    {
        return XPATH_ENGINE_ERR_OPEN;
    }
    for (int i = 0; i < nr; i++)
    {
        if (append_node_string(ns->nodeTab[i], val_buf, val_sz) != 0)
        {
            return XPATH_ENGINE_ERR_NON_SCALAR;
        }
        if (engine_fs_write_le_string(ram, out_h, val_buf) != 0)
        {
            return XPATH_ENGINE_ERR_OPEN;
        }
    }
    return 0;
}

static int xpath_eval_to_le_columns(
    xmlXPathContextPtr ctx,
    const char *expr,
    struct xfs_engine_mount_t *ram,
    struct xfs_handle_t *out_h,
    char *val_buf,
    size_t val_sz
)
{
    xmlXPathObjectPtr obj = xmlXPathEvalExpression(BAD_CAST expr, ctx);
    if (!obj)
    {
        SNX_CTRL_DEBUG("snx: xpath: eval failed expr='%s'\n", expr);
        return XPATH_ENGINE_ERR_XPATH;
    }

    int rc = 0;

    switch (obj->type)
    {
        case XPATH_NODESET:
            rc = xpath_write_nodeset(obj->nodesetval, ram, out_h, val_buf, val_sz);
            break;
        case XPATH_STRING: {
            const xmlChar *xs = obj->stringval;
            snprintf(val_buf, val_sz, "%s", xs ? (const char *)xs : "");
            engine_utf8_fold(val_buf, val_sz);
            if (engine_fs_write_le_string(ram, out_h, "1") != 0)
            {
                rc = XPATH_ENGINE_ERR_OPEN;
                break;
            }
            if (engine_fs_write_le_string(ram, out_h, val_buf) != 0)
            {
                rc = XPATH_ENGINE_ERR_OPEN;
            }
            break;
        }
        case XPATH_NUMBER: {
            const double d = obj->floatval;
            snprintf(val_buf, val_sz, "%.17g", d);
            engine_utf8_fold(val_buf, val_sz);
            if (engine_fs_write_le_string(ram, out_h, "1") != 0)
            {
                rc = XPATH_ENGINE_ERR_OPEN;
                break;
            }
            if (engine_fs_write_le_string(ram, out_h, val_buf) != 0)
            {
                rc = XPATH_ENGINE_ERR_OPEN;
            }
            break;
        }
        case XPATH_BOOLEAN: {
            snprintf(val_buf, val_sz, "%s", obj->boolval ? "true" : "false");
            engine_utf8_fold(val_buf, val_sz);
            if (engine_fs_write_le_string(ram, out_h, "1") != 0)
            {
                rc = XPATH_ENGINE_ERR_OPEN;
                break;
            }
            if (engine_fs_write_le_string(ram, out_h, val_buf) != 0)
            {
                rc = XPATH_ENGINE_ERR_OPEN;
            }
            break;
        }
        default:
            SNX_CTRL_DEBUG("snx: xpath: unsupported result type %d\n", (int)obj->type);
            rc = XPATH_ENGINE_ERR_NON_SCALAR;
            break;
    }

    xmlXPathFreeObject(obj);
    return rc;
}

int engine_xpath_call(const char *input_file, const char *output_file, int argc, char *argv[])
{
    int rc = 0;
    if (argc < 2)
    {
        SNX_CTRL_DEBUG("snx: xpath: invalid argc=%d\n", argc);
        return XPATH_ENGINE_ERR_XPATH;
    }

    if (engine_xpath_init() != 0)
        return XPATH_ENGINE_ERR_OPEN;

    int mount_index = 0;
    const char *path_in = NULL;
    if (engine_fs_parse_mount_spec(input_file, &mount_index, &path_in) != 0)
    {
        SNX_CTRL_DEBUG("snx: xpath: mount spec parse failed for '%s'\n", input_file);
        return XPATH_ENGINE_ERR_OPEN;
    }

    struct xfs_handle_t in_h = {};
    struct xfs_engine_mount_t *in_mnt = NULL;
    uint8_t *buf = NULL;
    size_t raw_len = 0;
    htmlDocPtr doc = NULL;
    struct xfs_engine_mount_t ram = {0};
    struct xfs_handle_t out_h = {};
    char *val_buf = NULL;
    int out_open = 0;

    if (engine_fs_open_read(mount_index, path_in, &in_h, &in_mnt) != 0)
    {
        SNX_CTRL_DEBUG("snx: xpath: open read failed mount=%d path='%s'\n", mount_index, path_in);
        return XPATH_ENGINE_ERR_OPEN;
    }

    if (engine_fs_read_entire(in_mnt, &in_h, &buf, &raw_len) != 0)
    {
        SNX_CTRL_DEBUG("snx: xpath: read failed path='%s'\n", path_in);
        rc = XPATH_ENGINE_ERR_OPEN;
        goto cleanup;
    }

    if (raw_len >= ENGINE_FS_READ_BUF_SIZE)
    {
        rc = XPATH_ENGINE_ERR_OPEN;
        goto cleanup;
    }
    buf[raw_len] = '\0';

    doc = htmlReadMemory((const char *)buf, (int)raw_len, input_file, NULL, HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (!doc)
    {
        SNX_CTRL_DEBUG("snx: xpath: HTML parse failed bytes=%zu\n", raw_len);
        rc = XPATH_ENGINE_ERR_PARSE;
        goto cleanup;
    }

    if (engine_fs_ram_mount(&ram) != 0)
    {
        rc = XPATH_ENGINE_ERR_OPEN;
        goto cleanup;
    }

    if (engine_fs_ram_open_write(&ram, &out_h, output_file) != 0)
    {
        rc = XPATH_ENGINE_ERR_OPEN;
        goto cleanup;
    }
    out_open = 1;

    val_buf = (char *)engine_xpath_alloc(4096u);
    if (!val_buf)
    {
        rc = XPATH_ENGINE_ERR_OPEN;
        goto cleanup;
    }

    xmlXPathContextPtr ctx = xmlXPathNewContext((xmlDocPtr)doc);
    if (!ctx)
    {
        rc = XPATH_ENGINE_ERR_PARSE;
        goto cleanup;
    }
    xpath_register_namespaces(ctx, (xmlDocPtr)doc);

    for (int pi = 1; pi < argc; pi++)
    {
        SNX_CTRL_DEBUG("snx: xpath: expr[%d]='%s'\n", pi - 1, argv[pi]);
        const int er = xpath_eval_to_le_columns(ctx, argv[pi], &ram, &out_h, val_buf, 4096u);
        if (er != 0)
        {
            rc = er;
            xmlXPathFreeContext(ctx);
            goto cleanup;
        }
    }

    xmlXPathFreeContext(ctx);

cleanup:
    if (val_buf)
    {
        engine_xpath_free(val_buf);
    }
    if (out_open)
    {
        engine_fs_close(&ram, &out_h);
    }
    if (doc)
    {
        xmlFreeDoc((xmlDocPtr)doc);
    }
    if (in_mnt)
    {
        engine_fs_close(in_mnt, &in_h);
    }
    return rc;
}

#else
int engine_xpath_call(const char *input_file, const char *output_file, int argc, char *argv[])
{
    (void)input_file;
    (void)output_file;
    (void)argc;
    (void)argv;
    return -1;
}
#endif
