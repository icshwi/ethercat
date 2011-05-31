#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <stddef.h>
#include <errno.h>
#include <sys/stat.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "classes.h"

typedef struct CONTEXT CONTEXT;
struct CONTEXT
{
    int depth;
    EC_CONFIG * config;
    EC_DEVICE_TYPE * device_type;
    EC_PDO * pdo;
    EC_PDO_ENTRY * pdo_entry;
    EC_SYNC_MANAGER * sync_manager;
    EC_DEVICE * device;
    EC_PDO_ENTRY_MAPPING * pdo_entry_mapping;
};

int expectNode(xmlNode * node, char * name)
{
    if((node->type == XML_ELEMENT_NODE) &&
       (strcmp((char *)node->name, name) == 0))
    {
        return 1;
    }
    else
    {
        printf("expected %s got %s", node->name, name);
        return 0;
    }
}

typedef int (*PARSEFUNC)(xmlNode *, CONTEXT *);

int parseChildren(xmlNode * node, CONTEXT * ctx, char * name, PARSEFUNC parseFunc)
{
    xmlNode * c;
    for(c = node->children; c; c = c->next)
    {
        if(c->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        if(!(expectNode(c, name) && parseFunc(c, ctx)))
        {
            return 0;
        }
    }
    return 1;
}

// octal constants are an absolute disaster, check them here
int isOctal(char * attr)
{
    enum { START, LEADING_ZERO } state = START;
    int n;
    for(n = 0; n < strlen(attr); n++)
    {
        if(isspace(attr[n]) || attr[n] == '+' || attr[n] == '-')
        {
            continue;
        }
        else if(state == START && attr[n] == '0')
        {
            state = LEADING_ZERO;
        }
        else if(state == LEADING_ZERO && isdigit(attr[n]))
        {
            return 1;
        }
        else
        {
            break;
        }
    }
    return 0;
}

int getInt(xmlNode * node, char * name, int * value, int required)
{
    char * text = (char *)xmlGetProp(node, (unsigned char *)name);
    if(text != NULL)
    {
        if(isOctal(text))
        {
            printf("error: constants with leading zeros are disallowed as they are parsed as octal\n");
            return 0;
        }
        errno = 0;
        *value = strtol(text, NULL, 0);
        return !errno;
    }
    else
    {
        if(required)
        {
            printf("can't find %s\n", name);
        }
        else
        {
            *value = 0;
            return 1;
        }
    }
    return 0;
}

int getStr(xmlNode * node, char * name, char ** value)
{
    *value = (char *)xmlGetProp(node, (unsigned char *)name);
    return (*value != NULL);
}

int parsePdoEntry(xmlNode * node, CONTEXT * ctx)
{
    ctx->pdo_entry = calloc(1, sizeof(EC_PDO_ENTRY));
    return 
        getInt(node, "index", &ctx->pdo_entry->index, 1) &&
        getInt(node, "subindex", &ctx->pdo_entry->sub_index, 1) &&
        getInt(node, "bit_length", &ctx->pdo_entry->bits, 1) &&
        getStr(node, "name", &ctx->pdo_entry->name) &&
        getStr(node, "datatype", &ctx->pdo_entry->datatype) &&
        getInt(node, "oversample", &ctx->pdo_entry->oversampling, 1) &&
        (ctx->pdo_entry->parent = ctx->pdo) && 
        listAdd(&ctx->pdo->pdo_entries, &ctx->pdo_entry->node);
}

int parsePdo(xmlNode * node, CONTEXT * ctx)
{
    ctx->pdo = calloc(1, sizeof(EC_PDO));
    return 
        getStr(node, "name", &ctx->pdo->name) &&
        getInt(node, "index", &ctx->pdo->index, 1) &&
        parseChildren(node, ctx, "entry", parsePdoEntry) &&
        (ctx->pdo->parent = ctx->sync_manager) && 
        listAdd(&ctx->sync_manager->pdos, &ctx->pdo->node);
}
             
int parseSync(xmlNode * node, CONTEXT * ctx)
{
    ctx->sync_manager = calloc(1, sizeof(EC_SYNC_MANAGER));
    return
        getInt(node, "index", &ctx->sync_manager->index, 1) &&
        getInt(node, "dir", &ctx->sync_manager->direction, 1) &&
        getInt(node, "watchdog", &ctx->sync_manager->watchdog, 1) &&
        parseChildren(node, ctx, "pdo", parsePdo) &&
        (ctx->sync_manager->parent = ctx->device_type) && 
        listAdd(&ctx->device_type->sync_managers, &ctx->sync_manager->node);
}

int parseDeviceType(xmlNode * node, CONTEXT * ctx)
{
    ctx->device_type = calloc(1, sizeof(EC_DEVICE_TYPE));
    return
        getStr(node, "name", &ctx->device_type->name) &&
        getInt(node, "dcactivate", &ctx->device_type->oversampling_activate, 0) &&
        getInt(node, "vendor", &ctx->device_type->vendor_id, 1) &&
        getInt(node, "product", &ctx->device_type->product_id, 1) &&
        parseChildren(node, ctx, "sync", parseSync) &&
        listAdd(&ctx->config->device_types, &ctx->device_type->node);
}

int parseTypes(xmlNode * node, CONTEXT * ctx)
{
    return parseChildren(node, ctx, "device", parseDeviceType);
}

// chain parser

int joinDevice(EC_CONFIG * cfg, EC_DEVICE * device)
{
    device->device_type = find_device_type(cfg, device->type_name);
    return (device->device_type != NULL);
}

int parseDevice(xmlNode * node, CONTEXT * ctx)
{
    ctx->device = calloc(1, sizeof(EC_DEVICE));
    return 
        getStr(node, "name", &ctx->device->name) &&
        getInt(node, "oversample", &ctx->device->oversampling_rate, 0) &&
        getStr(node, "type_name", &ctx->device->type_name) &&
        getInt(node, "position", &ctx->device->position, 1) &&
        joinDevice(ctx->config, ctx->device) &&
        listAdd(&ctx->config->devices, &ctx->device->node);
}

int parseChain(xmlNode * node, CONTEXT * ctx)
{
    return parseChildren(node, ctx, "device", parseDevice);
}

int joinPdoEntryMapping(EC_CONFIG * cfg, EC_PDO_ENTRY_MAPPING * mapping)
{
    mapping->parent = find_device(cfg, mapping->device_position);
    if(mapping->parent == NULL)
    {
        return 0;
    }
    mapping->pdo_entry = find_pdo_entry(mapping->parent, mapping->index, mapping->sub_index);
    if(mapping->pdo_entry == NULL)
    {
        return 0;
    }
    listAdd(&mapping->parent->pdo_entry_mappings, &mapping->node);
    return 1;
}

int parsePdoEntryMapping(xmlNode * node, CONTEXT * ctx)
{
    ctx->pdo_entry_mapping = calloc(1, sizeof(EC_PDO_ENTRY_MAPPING));
    return 
        getInt(node, "index", &ctx->pdo_entry_mapping->index, 1) &&
        getInt(node, "sub_index", &ctx->pdo_entry_mapping->sub_index, 1) &&
        getInt(node, "device_position", &ctx->pdo_entry_mapping->device_position, 1) &&
        getInt(node, "offset", &ctx->pdo_entry_mapping->offset, 1) &&
        getInt(node, "bit", &ctx->pdo_entry_mapping->bit_position, 1) &&
        joinPdoEntryMapping(ctx->config, ctx->pdo_entry_mapping);
}

int parseEntriesFromBuffer(char * text, int size, EC_CONFIG * cfg)
{
    CONTEXT ctx;
    ctx.config = cfg;
    //printf("%s\n", text);
    xmlDoc * doc = xmlReadMemory(text, size, NULL, NULL, 0);
    assert(doc);
    xmlNode * node = xmlDocGetRootElement(doc);
    assert(node);
    parseChildren(node, &ctx, "entry", parsePdoEntryMapping);
    return 0;
}

int dump(xmlNode * node, CONTEXT * ctx)
{
    printf("%s %d\n", node->name, node->type);
    xmlNode * c;
    for(c = node->children; c; c = c->next)
    {
        if(c->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        if(!dump(c, ctx))
        {
            return 0;
        }
    }
    return 1;
}

void show(CONTEXT * ctx)
{
    printf("device types %d\n", ctx->config->device_types.count);
    NODE * node;
    for(node = listFirst(&ctx->config->device_types); node; node = node->next)
    {
        EC_DEVICE_TYPE * device_type = 
            (EC_DEVICE_TYPE *)node;
        printf("sync managers %d\n", device_type->sync_managers.count);
    }
    printf("device instances %d\n", ctx->config->devices.count);
    for(node = listFirst(&ctx->config->devices); node; node = node->next)
    {
        EC_DEVICE * device = 
            (EC_DEVICE *)node;
        printf("name %s position %d\n", device->name, device->position);
    }
}

int read_config2(char * config, int size, EC_CONFIG * cfg)
{
    LIBXML_TEST_VERSION;
    xmlDoc * doc = xmlReadMemory(config, size, NULL, NULL, 0);
    assert(doc);
    CONTEXT ctx;
    ctx.config = cfg;
    xmlNode * node = xmlDocGetRootElement(doc);
    xmlNode * c;
    for(c = node->children; c; c = c->next)
    {
        if(c->type != XML_ELEMENT_NODE)
        {
            continue;
        }
        printf("%s\n", c->name);
        if(strcmp((char *)c->name, "devices") == 0)
        {
            parseTypes(c, &ctx);
        }
        else if(strcmp((char *)c->name, "chain") == 0)
        {
            parseChain(c, &ctx);
        }
    }
    return 0;
}

int read_config(char * config, char * chain, EC_CONFIG * cfg)
{
    LIBXML_TEST_VERSION;
    xmlDoc * doc = xmlReadFile(config, NULL, 0);
    assert(doc);
    CONTEXT ctx;
    ctx.config = cfg;
    if(parseTypes(xmlDocGetRootElement(doc), &ctx))
    {
        //show(&ctx);
    }
    else
    {
        printf("parse error\n");
    }
    xmlFreeDoc(doc);

    doc = xmlReadFile(chain, NULL, 0);
    assert(doc);
    if(parseChain(xmlDocGetRootElement(doc), &ctx))
    {
        //show(&ctx);
    }
    else
    {
        printf("chain parse error\n");
    }
    xmlFreeDoc(doc);
    xmlCleanupParser();
    return 0;
}

char * load_config(char * filename)
{
    struct stat st;
    if(stat(filename, &st) != 0)
    {
        fprintf(stderr, "failed to get size of %s: %s\n", filename, strerror(errno));
        exit(1);
    }
    void * buf = calloc(st.st_size + 1, sizeof(char));
    assert(buf);
    FILE * f = fopen(filename, "r");
    if(f == NULL)
    {
        fprintf(stderr, "failed to open %s: %s\n", filename, strerror(errno));
        exit(1);
    }
    assert(fread(buf, sizeof(char), st.st_size, f) == st.st_size);
    assert(fclose(f) == 0);
    return buf;
}

char * serialize_config(EC_CONFIG * cfg)
{
    /* serialize PDO mapping */
    int scount = 1024*1024;
    char * sbuf = calloc(scount, sizeof(char));
    strncat(sbuf, "<entries>\n", scount-strlen(sbuf)-1);
    NODE * node;
    for(node = listFirst(&cfg->devices); node; node = node->next)
    {
        EC_DEVICE * device = (EC_DEVICE *)node;
        NODE * node1;
        for(node1 = listFirst(&device->pdo_entry_mappings); node1; node1 = node1->next)
        {
            EC_PDO_ENTRY_MAPPING * pdo_entry_mapping = (EC_PDO_ENTRY_MAPPING *)node1;
            char line[1024];
            snprintf(line, sizeof(line), "<entry device_position=\"%d\" index=\"0x%x\" sub_index=\"0x%x\" offset=\"%d\" bit=\"%d\" />\n", 
                     device->position, pdo_entry_mapping->index, pdo_entry_mapping->sub_index, pdo_entry_mapping->offset, 
                     pdo_entry_mapping->bit_position);
            strncat(sbuf, line, scount-strlen(sbuf)-1);
        }
    }
    strncat(sbuf, "</entries>\n", scount-strlen(sbuf)-1);
    return sbuf;
}

int main_TEST_PARSER(int argc, char ** argv)
{
    LIBXML_TEST_VERSION;
    /*
    assert(argc > 1);
    EC_CONFIG * cfg = calloc(1, sizeof(EC_CONFIG));
    read_config2(argv[1], cfg);
    */
    return 0;
}