#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SnVal sn_val_unit(void) {
    SnVal v;
    v.tag = VAL_UNIT;
    v.as.i = 0;
    return v;
}

SnVal sn_val_bool(bool b) {
    SnVal v;
    v.tag = VAL_BOOL;
    v.as.b = b;
    return v;
}

SnVal sn_val_int(int64_t i) {
    SnVal v;
    v.tag = VAL_INT;
    v.as.i = i;
    return v;
}

SnVal sn_val_double(double d) {
    SnVal v;
    v.tag = VAL_DOUBLE;
    v.as.d = d;
    return v;
}

SnVal sn_val_string(const char *str, size_t len) {
    SnStringObj *s = (SnStringObj *)malloc(sizeof(SnStringObj));
    s->hdr.rc = 1;
    s->hdr.tag = VAL_STRING;
    s->len = len;
    s->chars = (char *)malloc(len + 1);
    if (str && len) {
        memcpy(s->chars, str, len);
    }
    s->chars[len] = '\0';

    SnVal v;
    v.tag = VAL_STRING;
    v.as.obj = (SnObjHeader *)s;
    return v;
}

SnVal sn_val_array(size_t initial_cap) {
    SnArrayObj *a = (SnArrayObj *)malloc(sizeof(SnArrayObj));
    a->hdr.rc = 1;
    a->hdr.tag = VAL_ARRAY;
    a->count = 0;
    a->capacity = initial_cap ? initial_cap : 8;
    a->items = (SnVal *)malloc(a->capacity * sizeof(SnVal));

    SnVal v;
    v.tag = VAL_ARRAY;
    v.as.obj = (SnObjHeader *)a;
    return v;
}

SnVal sn_val_instance(uint32_t class_id, size_t field_count) {
    SnInstanceObj *inst = (SnInstanceObj *)malloc(sizeof(SnInstanceObj));
    inst->hdr.rc = 1;
    inst->hdr.tag = VAL_OBJECT;
    inst->class_id = class_id;
    inst->field_count = field_count;
    inst->fields = field_count ? (SnVal *)calloc(field_count, sizeof(SnVal)) : NULL;

    SnVal v;
    v.tag = VAL_OBJECT;
    v.as.obj = (SnObjHeader *)inst;
    return v;
}

SnVal sn_val_variant(uint32_t tag_id, const char *name, size_t payload_count) {
    SnVariantObj *var = (SnVariantObj *)malloc(sizeof(SnVariantObj));
    var->hdr.rc = 1;
    var->hdr.tag = VAL_VARIANT;
    var->tag_id = tag_id;
    var->tag_name = name ? strdup(name) : NULL;
    var->payload_count = payload_count;
    var->payloads = payload_count ? (SnVal *)calloc(payload_count, sizeof(SnVal)) : NULL;

    SnVal v;
    v.tag = VAL_VARIANT;
    v.as.obj = (SnObjHeader *)var;
    return v;
}

void sn_val_retain(SnVal val) {
    if (val.tag >= VAL_STRING && val.as.obj) {
        val.as.obj->rc++;
    }
}

void sn_val_release(SnVal val) {
    if (val.tag < VAL_STRING || !val.as.obj) {
        return;
    }
    if (--val.as.obj->rc > 0) {
        return;
    }
    switch (val.tag) {
    case VAL_STRING: {
        SnStringObj *s = (SnStringObj *)val.as.obj;
        free(s->chars);
        free(s);
        break;
    }
    case VAL_ARRAY: {
        SnArrayObj *a = (SnArrayObj *)val.as.obj;
        for (size_t i = 0; i < a->count; i++) {
            sn_val_release(a->items[i]);
        }
        free(a->items);
        free(a);
        break;
    }
    case VAL_OBJECT: {
        SnInstanceObj *inst = (SnInstanceObj *)val.as.obj;
        for (size_t i = 0; i < inst->field_count; i++) {
            sn_val_release(inst->fields[i]);
        }
        free(inst->fields);
        free(inst);
        break;
    }
    case VAL_VARIANT: {
        SnVariantObj *var = (SnVariantObj *)val.as.obj;
        for (size_t i = 0; i < var->payload_count; i++) {
            sn_val_release(var->payloads[i]);
        }
        free(var->payloads);
        free(var->tag_name);
        free(var);
        break;
    }
    default:
        free(val.as.obj);
        break;
    }
}

bool sn_val_equal(SnVal a, SnVal b) {
    if (a.tag != b.tag) {
        if ((a.tag == VAL_INT && b.tag == VAL_DOUBLE)) {
            return (double)a.as.i == b.as.d;
        }
        if ((a.tag == VAL_DOUBLE && b.tag == VAL_INT)) {
            return a.as.d == (double)b.as.i;
        }
        return false;
    }
    switch (a.tag) {
    case VAL_UNIT:
        return true;
    case VAL_BOOL:
        return a.as.b == b.as.b;
    case VAL_INT:
        return a.as.i == b.as.i;
    case VAL_DOUBLE:
        return a.as.d == b.as.d;
    case VAL_STRING: {
        SnStringObj *sa = (SnStringObj *)a.as.obj;
        SnStringObj *sb = (SnStringObj *)b.as.obj;
        return sa->len == sb->len && strcmp(sa->chars, sb->chars) == 0;
    }
    case VAL_VARIANT: {
        SnVariantObj *va = (SnVariantObj *)a.as.obj;
        SnVariantObj *vb = (SnVariantObj *)b.as.obj;
        if (va->tag_id != vb->tag_id || va->payload_count != vb->payload_count) {
            return false;
        }
        for (size_t i = 0; i < va->payload_count; i++) {
            if (!sn_val_equal(va->payloads[i], vb->payloads[i])) {
                return false;
            }
        }
        return true;
    }
    default:
        return a.as.obj == b.as.obj;
    }
}

void sn_val_print(SnVal val, bool newline) {
    switch (val.tag) {
    case VAL_UNIT:
        printf("unit");
        break;
    case VAL_BOOL:
        printf("%s", val.as.b ? "true" : "false");
        break;
    case VAL_INT:
        printf("%ld", (long)val.as.i);
        break;
    case VAL_DOUBLE:
        printf("%g", val.as.d);
        break;
    case VAL_STRING: {
        SnStringObj *s = (SnStringObj *)val.as.obj;
        printf("%s", s ? s->chars : "");
        break;
    }
    case VAL_ARRAY: {
        SnArrayObj *a = (SnArrayObj *)val.as.obj;
        printf("[");
        if (a) {
            for (size_t i = 0; i < a->count; i++) {
                if (i > 0) printf(", ");
                sn_val_print(a->items[i], false);
            }
        }
        printf("]");
        break;
    }
    case VAL_OBJECT: {
        SnInstanceObj *inst = (SnInstanceObj *)val.as.obj;
        printf("<Instance #%u>", inst ? inst->class_id : 0);
        break;
    }
    case VAL_VARIANT: {
        SnVariantObj *var = (SnVariantObj *)val.as.obj;
        if (var) {
            printf("%s", var->tag_name ? var->tag_name : "Variant");
            if (var->payload_count > 0) {
                printf("(");
                for (size_t i = 0; i < var->payload_count; i++) {
                    if (i > 0) printf(", ");
                    sn_val_print(var->payloads[i], false);
                }
                printf(")");
            }
        }
        break;
    }
    }
    if (newline) {
        printf("\n");
    }
}
