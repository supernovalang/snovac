#include "builtins.h"

#include <string.h>

/* Where a member's result type comes from. Anything the member NAME alone
 * doesn't determine is B_ANY on purpose — see the header. */
typedef enum {
    B_UNIT,
    B_INT,
    B_BOOL,
    B_STRING,
    B_ANY,
    B_ELEM,         /* the receiver array's element type */
    B_SELF,         /* the receiver type unchanged */
    B_ARRAY_ANY,    /* Array<any> */
    B_ARRAY_STRING  /* Array<string> */
} BSlot;

typedef struct {
    const char *name;
    BSlot ret;
} BMember;

/* crates/snovalang/src/native/selfcheck/static_facts.rs, "Array" entry, in the
 * same order. Result types are this module's addition — that table has none. */
static const BMember ARRAY_MEMBERS[] = {
    {"length", B_INT},
    {"len", B_INT},
    {"push", B_UNIT},
    {"pop", B_ELEM},
    {"clear", B_UNIT},
    {"isEmpty", B_BOOL},
    {"get", B_ELEM},
    {"set", B_UNIT},
    {"map", B_ARRAY_ANY},
    {"filter", B_SELF},
    {"forEach", B_UNIT},
    {"reduce", B_ANY},
    {"flatMap", B_ARRAY_ANY},
    {"take", B_SELF},
    {"drop", B_SELF},
    {"first", B_ELEM},
    {"last", B_ELEM},
    {"join", B_STRING},
    {"concurrentMapping", B_ARRAY_ANY},
    {"concurrentMappingWith", B_ARRAY_ANY},
    {"concurrentForEach", B_UNIT},
};

/* Same source, "string" entry. `charAt`/`substring` come back as `string`
 * rather than `char`: the corpus indexes strings and compares the result
 * against string literals, and `char` in Snovalang is written with a char
 * literal, never produced by these. */
static const BMember STRING_MEMBERS[] = {
    {"length", B_INT},
    {"len", B_INT},
    {"toString", B_STRING},
    {"asString", B_STRING},
    {"charAt", B_STRING},
    {"substring", B_STRING},
    {"trim", B_STRING},
    {"trimStart", B_STRING},
    {"trimEnd", B_STRING},
    {"startsWith", B_BOOL},
    {"endsWith", B_BOOL},
    {"contains", B_BOOL},
    {"indexOf", B_INT},
    {"lastIndexOf", B_INT},
    {"split", B_ARRAY_STRING},
    {"replace", B_STRING},
    {"isEmpty", B_BOOL},
};

/* Same source: int/long/decimal/float/double/bool all carry exactly these. */
static const BMember SCALAR_MEMBERS[] = {
    {"toString", B_STRING},
    {"asString", B_STRING},
};

static SnTypeRep *slot_type(SnTypeTable *t, BSlot slot, const SnTypeRep *recv) {
    switch (slot) {
    case B_UNIT:
        return sn_type_unit(t);
    case B_INT:
        return sn_type_int(t);
    case B_BOOL:
        return sn_type_bool(t);
    case B_STRING:
        return sn_type_string(t);
    case B_ANY:
        return sn_type_any(t);
    case B_ELEM:
        return (recv->tag == SN_T_ARRAY && recv->nargs == 1) ? recv->args[0]
                                                             : sn_type_any(t);
    case B_SELF:
        return (SnTypeRep *)recv;
    case B_ARRAY_ANY:
        return sn_type_array(t, sn_type_any(t));
    case B_ARRAY_STRING:
        return sn_type_array(t, sn_type_string(t));
    }
    return sn_type_any(t);
}

/* Methods carry no parameter list — see the header. */
static SnTypeRep *method_of(SnTypeTable *t, BSlot ret, const SnTypeRep *recv) {
    return sn_type_func(t, NULL, 0, slot_type(t, ret, recv));
}

static const BMember *find_member(const BMember *table, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

SnTypeRep *sn_builtin_generic(SnTypeTable *t, SnInternTable *it, const char *iname,
                              SnTypeRep **args, uint32_t nargs) {
    if (iname == sn_intern_cstr(it, "Array")) {
        return sn_type_array(t, nargs >= 1 ? args[0] : sn_type_any(t));
    }
    if (iname == sn_intern_cstr(it, "Partial")) {
        /* Partial<T> is checked as T; bare `Partial` has nothing to stand in
         * for, so it degrades to `any` rather than inventing a shape. */
        return nargs >= 1 ? args[0] : sn_type_any(t);
    }
    if (iname == sn_intern_cstr(it, "Option") || iname == sn_intern_cstr(it, "Result")) {
        static SnSymbol opt_sym = { .name = "Option", .kind = SN_SYM_TYPE };
        static SnSymbol res_sym = { .name = "Result", .kind = SN_SYM_TYPE };
        SnSymbol *sym = (iname == sn_intern_cstr(it, "Option")) ? &opt_sym : &res_sym;
        return sn_type_named(t, sym, args, nargs);
    }
    return NULL;
}

int sn_builtin_is_generic_name(SnInternTable *it, const char *iname) {
    return iname == sn_intern_cstr(it, "Array") || iname == sn_intern_cstr(it, "Partial") ||
           iname == sn_intern_cstr(it, "Option") || iname == sn_intern_cstr(it, "Result");
}

SnTypeRep *sn_builtin_member(SnTypeTable *t, SnInternTable *it, const SnTypeRep *recv,
                             const char *iname) {
    (void)it;
    if (!recv) {
        return NULL;
    }

    const BMember *m = NULL;
    switch (recv->tag) {
    case SN_T_ARRAY:
        m = find_member(ARRAY_MEMBERS, sizeof(ARRAY_MEMBERS) / sizeof(ARRAY_MEMBERS[0]),
                        iname);
        break;
    case SN_T_STRING:
        m = find_member(STRING_MEMBERS, sizeof(STRING_MEMBERS) / sizeof(STRING_MEMBERS[0]),
                        iname);
        break;
    case SN_T_INT:
    case SN_T_LONG:
    case SN_T_DOUBLE:
    case SN_T_DECIMAL:
    case SN_T_FLOAT:
    case SN_T_BYTE:
    case SN_T_BOOL:
    case SN_T_CHAR:
        m = find_member(SCALAR_MEMBERS, sizeof(SCALAR_MEMBERS) / sizeof(SCALAR_MEMBERS[0]),
                        iname);
        break;
    default:
        return NULL;
    }
    return m ? method_of(t, m->ret, recv) : NULL;
}

SnTypeRep *sn_builtin_index_result(SnTypeTable *t, SnInternTable *it,
                                   const SnTypeRep *recv) {
    if (!recv) {
        return NULL;
    }
    if (recv->tag == SN_T_ARRAY) {
        return recv->nargs == 1 ? recv->args[0] : sn_type_any(t);
    }
    if (recv->tag == SN_T_STRING) {
        return sn_type_string(t); /* same choice as charAt — see STRING_MEMBERS */
    }
    if (recv->tag != SN_T_NAMED || !recv->decl) {
        return NULL;
    }
    const char *name = recv->decl->name;
    if (name == sn_intern_cstr(it, "Map")) {
        /* Map<K, V> yields V. */
        return recv->nargs == 2 ? recv->args[1] : sn_type_any(t);
    }
    if (name == sn_intern_cstr(it, "List")) {
        return recv->nargs == 1 ? recv->args[0] : sn_type_any(t);
    }
    if (name == sn_intern_cstr(it, "Bytes") || name == sn_intern_cstr(it, "JsonValue") ||
        name == sn_intern_cstr(it, "JsonArray")) {
        return sn_type_any(t);
    }
    return NULL;
}

SnTypeRep *sn_builtin_static_member(SnTypeTable *t, SnInternTable *it,
                                    const SnTypeRep *recv, const char *iname) {
    (void)it;
    if (!recv || recv->tag != SN_T_ARRAY) {
        return NULL;
    }
    /* `Array<Post>.new()` — tests/compile-pass/pipeline-element-type.snova.
     * The only static member the corpus uses on an intrinsic. */
    if (strcmp(iname, "new") == 0) {
        return sn_type_func(t, NULL, 0, (SnTypeRep *)recv);
    }
    return NULL;
}
