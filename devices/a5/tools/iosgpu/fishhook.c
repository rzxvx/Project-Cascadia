// fishhook.c -- from facebook/fishhook (MIT), trimmed to the 32-bit Mach-O
// (armv7) path this project needs.  Rebinds lazy and non-lazy symbol pointers
// across every loaded image, so a call another dylib makes to, say,
// IOConnectCallMethod goes to our replacement instead.
//
// Copyright (c) 2013, Facebook, Inc.  MIT licence; see the upstream repo.
#include "fishhook.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

// 32-bit Mach-O
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST "__DATA_CONST"   /* not present on iOS 8; never matches */
#endif

struct rebindings_entry {
    struct rebinding *rebindings;
    size_t rebindings_nel;
    struct rebindings_entry *next;
};

static struct rebindings_entry *_rebindings_head;

static int prepend_rebindings(struct rebindings_entry **head,
                              struct rebinding rebindings[], size_t nel)
{
    struct rebindings_entry *entry = malloc(sizeof(struct rebindings_entry));
    if (!entry)
        return -1;
    entry->rebindings = malloc(sizeof(struct rebinding) * nel);
    if (!entry->rebindings) {
        free(entry);
        return -1;
    }
    memcpy(entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
    entry->rebindings_nel = nel;
    entry->next = *head;
    *head = entry;
    return 0;
}

static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                           section_t *section,
                                           intptr_t slide,
                                           nlist_t *symtab,
                                           char *strtab,
                                           uint32_t *indirect_symtab)
{
    uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
    void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
    for (uint32_t i = 0; i < section->size / sizeof(void *); i++) {
        uint32_t symtab_index = indirect_symbol_indices[i];
        if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
            symtab_index == (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))
            continue;
        uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
        char *symbol_name = strtab + strtab_offset;
        if (strlen(symbol_name) < 2)
            continue;
        struct rebindings_entry *cur = rebindings;
        while (cur) {
            for (uint32_t j = 0; j < cur->rebindings_nel; j++) {
                if (strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
                    if (cur->rebindings[j].replaced &&
                        indirect_symbol_bindings[i] != cur->rebindings[j].replacement)
                        *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];
                    indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
                    goto symbol_loop;
                }
            }
            cur = cur->next;
        }
    symbol_loop:;
    }
}

static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                     const struct mach_header *header,
                                     intptr_t slide)
{
    Dl_info info;
    if (dladdr(header, &info) == 0)
        return;

    segment_command_t *cur_seg_cmd;
    segment_command_t *linkedit_segment = NULL;
    struct symtab_command *symtab_cmd = NULL;
    struct dysymtab_command *dysymtab_cmd = NULL;

    uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
    for (uint32_t i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
        cur_seg_cmd = (segment_command_t *)cur;
        if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
            if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0)
                linkedit_segment = cur_seg_cmd;
        } else if (cur_seg_cmd->cmd == LC_SYMTAB) {
            symtab_cmd = (struct symtab_command *)cur_seg_cmd;
        } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
            dysymtab_cmd = (struct dysymtab_command *)cur_seg_cmd;
        }
    }
    if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
        !dysymtab_cmd->nindirectsyms)
        return;

    uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
    nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
    char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);
    uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    cur = (uintptr_t)header + sizeof(mach_header_t);
    for (uint32_t i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
        cur_seg_cmd = (segment_command_t *)cur;
        if (cur_seg_cmd->cmd != LC_SEGMENT_ARCH_DEPENDENT)
            continue;
        if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
            strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0)
            continue;
        for (uint32_t j = 0; j < cur_seg_cmd->nsects; j++) {
            section_t *sect = (section_t *)(cur + sizeof(segment_command_t)) + j;
            if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS ||
                (sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS)
                perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab,
                                               indirect_symtab);
        }
    }
}

static void _rebind_symbols_for_image(const struct mach_header *header, intptr_t slide)
{
    rebind_symbols_for_image(_rebindings_head, header, slide);
}

int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel)
{
    int ret = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel);
    if (ret < 0)
        return ret;
    if (_rebindings_head->next == NULL) {
        _dyld_register_func_for_add_image(_rebind_symbols_for_image);
    } else {
        uint32_t c = _dyld_image_count();
        for (uint32_t i = 0; i < c; i++)
            _rebind_symbols_for_image(_dyld_get_image_header(i),
                                      _dyld_get_image_vmaddr_slide(i));
    }
    return ret;
}
