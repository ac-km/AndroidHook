//
// Created by yanxq on 17/6/10.
//
#include <string>
#include <fcntl.h>
#include <elf.h>
#include "elfhook_utils.h"
#include "elf_log.h"

static inline Elf32_Phdr* find_segment_by_type(Elf32_Phdr * phdr_base, const Elf32_Word phnum, const Elf32_Word ph_type) {
    Elf32_Phdr *target = NULL;
    for (int i = 0; i < phnum; ++i) {
        if ((phdr_base + i)->p_type == ph_type) {
            target = phdr_base + i;
            break;
        }
    }
    return target;
}

static inline Elf32_Addr * find_symbol_offset(const char *symbol,
                                              Elf32_Rel *rel_base_ptr,Elf32_Word size,
                                              const char *strtab_base,Elf32_Sym *symtab_ptr) {
    Elf32_Rel *each_rel = rel_base_ptr;
    size_t  symbol_length = strlen(symbol);
    for (int i = 0; i < size; i++,each_rel++) {
        uint16_t ndx = ELF32_R_SYM(each_rel->r_info);
        if (ndx == 0) continue;
        LOGI("ndx = %d, str = %s", ndx, strtab_base + symtab_ptr[ndx].st_name);
        if (memcmp(strtab_base + symtab_ptr[ndx].st_name, symbol,symbol_length) == 0) {
            LOGI("符号%s在got表的偏移地址为: 0x%x", symbol, each_rel->r_offset);
            return  &each_rel->r_offset;
        }
    }
    return NULL;
}


uint elfhook_p(const char *so_name,const char *symbol, void *new_func_addr,void **origin_func_addr_ptr) {
    LOGI("%s hook starting...",so_name);
    uint8_t * elf_base_address = (uint8_t *) find_so_base(so_name, NULL,0);
    if (elf_base_address == 0) {
        LOGE("Find %s base address failed!!!", so_name);
        return 0;
    }

    Elf32_Ehdr *endr = reinterpret_cast<Elf32_Ehdr*>(elf_base_address);
    if ((endr->e_version != EV_CURRENT && endr->e_version != EV_NONE)
        || (endr->e_type != ET_DYN && endr->e_type != ET_EXEC)) {
        LOGE("Find error ehdr!!! version: %d, type: %d", endr->e_version, endr->e_type);
        return 0;
    }

    Elf32_Phdr *phdr_base = reinterpret_cast<Elf32_Phdr*>(elf_base_address + endr->e_phoff);

    Elf32_Phdr *dynamic_phdr = find_segment_by_type(phdr_base,endr->e_phnum,PT_DYNAMIC);
    Elf32_Dyn *dyn_ptr_base = reinterpret_cast<Elf32_Dyn *>(elf_base_address + dynamic_phdr->p_vaddr);
    Elf32_Word dynamic_size = dynamic_phdr -> p_memsz;

    Elf32_Word dyn_count = dynamic_size / sizeof(Elf32_Dyn);

    Elf32_Sym *symtab_ptr = NULL;
    const char * strtab_base = NULL;
    Elf32_Rel *rel_dyn_ptr_base = NULL;
    Elf32_Word rel_dyn_count;
    Elf32_Rel *rel_plt_base = NULL;
    Elf32_Word  rel_plt_count;

    Elf32_Word current_find_count = 0;
    Elf32_Dyn * each_dyn = dyn_ptr_base;
    for (int i = 0; i < dyn_count; ++i,++each_dyn) {
        switch (each_dyn->d_tag) {
            case DT_SYMTAB:
                symtab_ptr = reinterpret_cast<Elf32_Sym *>(elf_base_address + each_dyn->d_un.d_ptr);
                current_find_count ++;
                break;
            case DT_STRTAB:
                current_find_count ++;
                strtab_base = reinterpret_cast<const char *>(elf_base_address + each_dyn->d_un.d_ptr);
                break;
            case DT_REL:
                current_find_count ++;
                rel_dyn_ptr_base = reinterpret_cast<Elf32_Rel *>(elf_base_address + each_dyn->d_un.d_ptr);
                break;
            case DT_RELASZ:
                current_find_count ++;
                rel_dyn_count = each_dyn->d_un.d_ptr / sizeof(Elf32_Rel);
                break;
            case DT_JMPREL:
                current_find_count ++;
                rel_plt_base = reinterpret_cast<Elf32_Rel *>(elf_base_address + each_dyn->d_un.d_ptr);
                break;
            case DT_PLTRELSZ:
                current_find_count ++;
                rel_plt_count = each_dyn->d_un.d_ptr / sizeof(Elf32_Rel);
                break;
            default:
                break;
        }
        if (current_find_count == 5) {
            break;
        }
    }

    Elf32_Addr *offset = find_symbol_offset(symbol, rel_plt_base, rel_plt_count,
                                            strtab_base,symtab_ptr);
    if (offset == NULL) {
        LOGI(".rel.plt 查找符号失败，在 .rel.dyn 尝试查找...");
        offset = find_symbol_offset(symbol,rel_dyn_ptr_base,rel_dyn_count,
                                    strtab_base,symtab_ptr);
    }

    if (offset == NULL) {
        LOGE("获取 Offset 失败！！！");
        return 0;
    }

    LOGI("符号获取成功，进行符号地址修改...");
    void * function_addr_ptr = (elf_base_address + *offset);
    return replace_function((void **) function_addr_ptr,
                            new_func_addr, origin_func_addr_ptr);
}


static inline void read_data_form_fd(int fd, uint32_t seek,void * ptr, size_t size) {
    lseek(fd, seek, SEEK_SET);
    read(fd, ptr, size);
}

static inline Elf32_Word *find_symbol_offset(int fd,const char *symbol,
                                             Elf32_Rel *rel_base_ptr,Elf32_Word size,
                                             const char *strtab_base,Elf32_Sym *symtab_ptr) {

    size_t symbol_length = strlen(symbol);
    Elf32_Rel *each_rel = rel_base_ptr;
    for (uint16_t i = 0; i < size; i++) {
        uint16_t ndx = ELF32_R_SYM(each_rel->r_info);
        if (ndx > 0) {
            LOGI("ndx = %d, str = %s", ndx, strtab_base + symtab_ptr[ndx].st_name);
            if (memcmp(strtab_base + symtab_ptr[ndx].st_name, symbol,symbol_length) == 0) {
                LOGI("符号%s在got表的偏移地址为: 0x%x", symbol, each_rel->r_offset);
                return &each_rel->r_offset;
            }
        }
        if (read(fd, each_rel, sizeof(Elf32_Rel)) != sizeof(Elf32_Rel)) {
            LOGI("获取符号%s的重定位信息失败", symbol);
            return NULL;
        }
    }
    return NULL;
}

uint elfhook_s(const char *so_name,const char *symbol, void *new_func_addr,void **origin_func_addr_ptr) {
    char so_path[256] = {0};
    uint8_t * elf_base_address = (uint8_t *) find_so_base(so_name, so_path, sizeof(so_path));
    if (elf_base_address == 0) {
        LOGE("Find %s base address failed!!!", so_name);
        return 0;
    }

    //section 信息需要从 SO 文件中读取，因为该链接视图仅在 编译链接阶段有用，在执行中无用，
    //因此加载到内存后不一定有 section 段;
    int fd = open(so_path, O_RDONLY);

    //读取 ELF HEADER
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *) malloc(sizeof(Elf32_Ehdr));
    read(fd, ehdr, sizeof(Elf32_Ehdr));

    //查找 .shstrtab section，这个 section 存放各个 section 的名字，
    //我们需要通过它来找到我们需要的 section。
    uint32_t shdr_base = ehdr->e_shoff;
    uint16_t shnum = ehdr->e_shnum;
    uint32_t shstr_base = shdr_base + ehdr->e_shstrndx * sizeof(Elf32_Shdr);
    Elf32_Shdr *shstr = (Elf32_Shdr *) malloc(sizeof(Elf32_Shdr));
    read_data_form_fd(fd,shstr_base,shstr, sizeof(Elf32_Shdr));

    //定位 shstrtab 中  section name 字符的首地址
    char *shstrtab = (char *) malloc(shstr->sh_size);
    read_data_form_fd(fd,shstr->sh_offset,shstrtab,shstr->sh_size);

    //跳转到 section 开头，我们开始 section 遍历，通过 section 的 sh_name 可以在
    //shstrtab 中对照找到该 section 的名字，然后判断是不是我们需要的 section.
    Elf32_Shdr *shdr = (Elf32_Shdr *) malloc(sizeof(Elf32_Shdr));
    lseek(fd, shdr_base, SEEK_SET);

    /**
     * .rel.plt 保存外部符号的重定位信息, .dynsym 保存所有符号信息，
     * .dynstr 保存有符号的对应字符串表示;
     *
     * 我们需要修改目标符号在 .rel.plt 的重定位，但首先我们需要知道 .rel.plt 中哪一条是在说明目标符号的;
     * 定位的方法是，遍历 .rel.plt 的每一条，逐条拿出来查找它在 .dynsym 的对应详细信息，
     * .dynsym 的符号详细信息可以指引我们在 .dynstr 找到该符号的 name，通过比对 name 就能判断 .rel.plt 的条目是不是在说明我们目标符号的重定位;
     */
    char *sh_name = NULL;
    Elf32_Shdr *relplt_shdr = (Elf32_Shdr *) malloc(sizeof(Elf32_Shdr));
    Elf32_Shdr *dynsym_shdr = (Elf32_Shdr *) malloc(sizeof(Elf32_Shdr));
    Elf32_Shdr *dynstr_shdr = (Elf32_Shdr *) malloc(sizeof(Elf32_Shdr));
    Elf32_Shdr *reldyn_shdr = (Elf32_Shdr *) malloc(sizeof(Elf32_Shdr));
    for (uint16_t i = 0; i < shnum; ++i) {
        read(fd, shdr, sizeof(Elf32_Shdr));
        sh_name = shstrtab + shdr->sh_name;
        if (strcmp(sh_name, ".dynsym") == 0)
            memcpy(dynsym_shdr, shdr, sizeof(Elf32_Shdr));
        else if (strcmp(sh_name, ".dynstr") == 0)
            memcpy(dynstr_shdr, shdr, sizeof(Elf32_Shdr));
        else if (strcmp(sh_name, ".rel.plt") == 0)
            memcpy(relplt_shdr, shdr, sizeof(Elf32_Shdr));
        else if (strcmp(sh_name,".rel.dyn") == 0) {
            memcpy(reldyn_shdr,shdr, sizeof(Elf32_Shdr));
        }
    }

    //读取字符表
    char *dynstr = (char *) malloc(sizeof(char) * dynstr_shdr->sh_size);
    lseek(fd, dynstr_shdr->sh_offset, SEEK_SET);
    if (read(fd, dynstr, dynstr_shdr->sh_size) != dynstr_shdr->sh_size)
        return 0;

    //读取符号表
    Elf32_Sym *dynsymtab = (Elf32_Sym *) malloc(dynsym_shdr->sh_size);
    printf("dynsym_shdr->sh_size\t0x%x\n", dynsym_shdr->sh_size);
    lseek(fd, dynsym_shdr->sh_offset, SEEK_SET);
    if (read(fd, dynsymtab, dynsym_shdr->sh_size) != dynsym_shdr->sh_size)
        return 0;

    //读取重定位表
    Elf32_Rel *rel_ent = (Elf32_Rel *) malloc(sizeof(Elf32_Rel));
    lseek(fd, relplt_shdr->sh_offset, SEEK_SET);
    if (read(fd, rel_ent, sizeof(Elf32_Rel)) != sizeof(Elf32_Rel))
        return 0;

    LOGI("ELF 表准备完成, 开始查找符号%s的 got 表重定位地址...",symbol);
    Elf32_Addr *offset = find_symbol_offset(fd,symbol,rel_ent,
                                            relplt_shdr->sh_size / sizeof(Elf32_Rel),
                                            dynstr,dynsymtab);

    if (offset == NULL) {
        LOGI(".rel.plt 查找符号失败，在 .rel.dyn 尝试查找...");

        //读取重定向变量表
        Elf32_Rel *rel_dyn = (Elf32_Rel *) malloc(sizeof(Elf32_Rel));
        lseek(fd, reldyn_shdr->sh_offset, SEEK_SET);
        if (read(fd, rel_dyn, sizeof(Elf32_Rel)) != sizeof(Elf32_Rel))
            return 0;

        offset = find_symbol_offset(fd,symbol,rel_ent,
                                    relplt_shdr->sh_size / sizeof(Elf32_Rel),
                                    dynstr,dynsymtab);
    }

    if (offset == 0) {
        LOGE("符号%s地址获取失败", symbol);
        return 0;
    }

    LOGI("符号获取成功，进行符号地址修改...");
    void * function_addr_ptr = (elf_base_address + *offset);
    return replace_function((void **) function_addr_ptr,
                     new_func_addr, origin_func_addr_ptr);
}


uint elfhook_stop(uint symbol_offset,void **origin_func_addr_ptr) {
    return replace_function(reinterpret_cast<void **>(symbol_offset),*origin_func_addr_ptr,NULL);
}