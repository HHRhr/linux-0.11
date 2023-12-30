#ifndef _HEAD_H
#define _HEAD_H

					# 在32位系统中,long占用4字节,因此这里a和b分别是4字节长度,即单个表项占用8字节
typedef struct desc_struct {
	unsigned long a,b;
} desc_table[256];

extern unsigned long pg_dir[1024];
extern desc_table idt,gdt;

#define GDT_NUL 0
#define GDT_CODE 1
#define GDT_DATA 2
#define GDT_TMP 3

#define LDT_NUL 0
#define LDT_CODE 1
#define LDT_DATA 2

#endif
