/* dump.c — AST printing for `snovac --emit=ast`. */
#include "dump.h"

#include <stdio.h>

void sn_dump_type(const SnType *t) {
    if (!t) {
        printf("?");
        return;
    }
    if (t->kind == SN_TYPE_FUNC || t->kind == SN_TYPE_TUPLE) {
        printf("(");
        for (size_t i = 0; i < t->params.len; i++) {
            if (i) printf(", ");
            sn_dump_type((const SnType *)t->params.items[i]);
        }
        printf(")");
        if (t->kind == SN_TYPE_FUNC) {
            printf(" -> ");
            sn_dump_type(t->ret);
        }
        return;
    }
    printf("%s", t->name ? t->name : "?");
    if (t->args.len) {
        printf("<");
        for (size_t i = 0; i < t->args.len; i++) {
            if (i) printf(", ");
            sn_dump_type((const SnType *)t->args.items[i]);
        }
        printf(">");
    }
}

static const char *vis_name(SnVisibility v) {
    switch (v) {
    case SN_VIS_PUBLIC: return "public ";
    case SN_VIS_PRIVATE: return "private ";
    case SN_VIS_PROTECTED: return "protected ";
    default: return "";
    }
}

static void indent(int n) {
    for (int i = 0; i < n; i++) {
        printf("  ");
    }
}

static void print_header(const SnDecl *d) {
    switch (d->kind) {
    case SN_DECL_CLASS:     printf("%sclass %s", vis_name(d->vis), d->name); break;
    case SN_DECL_STRUCT:    printf("%sstruct %s", vis_name(d->vis), d->name); break;
    case SN_DECL_ENUM:      printf("%senum %s", vis_name(d->vis), d->name); break;
    case SN_DECL_INTERFACE: printf("%sinterface %s", vis_name(d->vis), d->name); break;
    case SN_DECL_METHOD:
    case SN_DECL_FUNC:
        printf("%s%s%s%s %s", vis_name(d->vis), d->is_static ? "static " : "",
               d->is_async ? "async " : "",
               d->kind == SN_DECL_METHOD ? "method" : "func", d->name);
        break;
    case SN_DECL_FIELD:
        printf("%s%s %s", vis_name(d->vis), d->is_mutable ? "var" : "let",
               d->name);
        break;
    case SN_DECL_CONST:     printf("%sconst %s", vis_name(d->vis), d->name); break;
    case SN_DECL_VARIANT:   printf("variant %s", d->name); break;
    case SN_DECL_TYPEALIAS: printf("%stype %s", vis_name(d->vis), d->name); break;
    case SN_DECL_EXTENSION: printf("%sextension %s", vis_name(d->vis), d->name); break;
    }
}

static void print_generics(const SnDecl *d) {
    if (!d->generics.len) {
        return;
    }
    printf("<");
    for (size_t i = 0; i < d->generics.len; i++) {
        if (i) printf(", ");
        printf("%s", (const char *)d->generics.items[i]);
    }
    printf(">");
}

static void print_params(const SnDecl *d) {
    if (d->kind != SN_DECL_METHOD && d->kind != SN_DECL_FUNC &&
        d->kind != SN_DECL_VARIANT) {
        return;
    }
    printf("(");
    for (size_t i = 0; i < d->params.len; i++) {
        const SnParam *pm = (const SnParam *)d->params.items[i];
        if (i) printf(", ");
        printf("%s: ", pm->name);
        sn_dump_type(pm->type);
        if (pm->def) printf(" = ...");
    }
    printf(")");
}

static void print_decorators(const SnDecl *d) {
    if (!d->decorators.len) {
        return;
    }
    printf("   [");
    for (size_t i = 0; i < d->decorators.len; i++) {
        if (i) printf(" ");
        printf("@%s", ((const SnDecorator *)d->decorators.items[i])->name);
    }
    printf("]");
}

void sn_dump_decl(const SnDecl *d, int depth) {
    indent(depth);
    print_header(d);
    print_generics(d);
    print_params(d);

    if (d->ret) {
        printf(": ");
        sn_dump_type(d->ret);
    }
    if (d->type) {
        printf(": ");
        sn_dump_type(d->type);
    }
    print_decorators(d);

    if ((d->kind == SN_DECL_METHOD || d->kind == SN_DECL_FUNC) && !d->body) {
        printf("   (no body — expects @native)");
    }
    printf("\n");

    for (size_t i = 0; i < d->variants.len; i++) {
        sn_dump_decl((const SnDecl *)d->variants.items[i], depth + 1);
    }
    for (size_t i = 0; i < d->members.len; i++) {
        sn_dump_decl((const SnDecl *)d->members.items[i], depth + 1);
    }
}

void sn_dump_unit(const SnUnit *unit) {
    if (unit->package) {
        printf("package %s\n", unit->package);
    }
    for (size_t i = 0; i < unit->imports.len; i++) {
        printf("import %s\n", (const char *)unit->imports.items[i]);
    }
    if (unit->imports.len || unit->package) {
        printf("\n");
    }
    for (size_t i = 0; i < unit->decls.len; i++) {
        sn_dump_decl((const SnDecl *)unit->decls.items[i], 0);
    }
}
