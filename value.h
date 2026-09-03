/* value.h — runtime value representation for Snovalang bytecode VM. */
#ifndef SNOVAC_VALUE_H
#define SNOVAC_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    VAL_UNIT = 0,
    VAL_BOOL,
    VAL_INT,
    VAL_DOUBLE,
    VAL_STRING,
    VAL_ARRAY,
    VAL_OBJECT,
    VAL_VARIANT
} SnValTag;

typedef struct SnObjHeader SnObjHeader;

typedef struct {
    SnValTag tag;
    union {
        bool b;
        int64_t i;
        double d;
        SnObjHeader *obj;
    } as;
} SnVal;

struct SnObjHeader {
    uint32_t rc;
    SnValTag tag;
};

typedef struct {
    SnObjHeader hdr;
    char *chars;
    size_t len;
} SnStringObj;

typedef struct {
    SnObjHeader hdr;
    SnVal *items;
    size_t count;
    size_t capacity;
} SnArrayObj;

typedef struct {
    SnObjHeader hdr;
    uint32_t class_id;
    SnVal *fields;
    size_t field_count;
} SnInstanceObj;

typedef struct {
    SnObjHeader hdr;
    uint32_t tag_id;
    char *tag_name;
    SnVal *payloads;
    size_t payload_count;
} SnVariantObj;

SnVal sn_val_unit(void);
SnVal sn_val_bool(bool b);
SnVal sn_val_int(int64_t i);
SnVal sn_val_double(double d);
SnVal sn_val_string(const char *str, size_t len);
SnVal sn_val_array(size_t initial_cap);
SnVal sn_val_instance(uint32_t class_id, size_t field_count);
SnVal sn_val_variant(uint32_t tag_id, const char *name, size_t payload_count);

void sn_val_retain(SnVal val);
void sn_val_release(SnVal val);
bool sn_val_equal(SnVal a, SnVal b);
void sn_val_print(SnVal val, bool newline);

#endif /* SNOVAC_VALUE_H */
