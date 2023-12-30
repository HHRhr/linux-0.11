;
;	setup.s		(C) 1991 Linus Torvalds
;
; setup.s is responsible for getting the system data from the BIOS,
; and putting them into the appropriate places in system memory.
; both setup.s and system has been loaded by the bootblock.
;
; This code asks the bios for memory/disk/other parameters, and
; puts them in a "safe" place: 0x90000-0x901FF, ie where the
; boot-block used to be. It is then up to the protected mode
; system to read them from there before the area is overwritten
; for buffer-blocks.
;

; NOTE; These had better be the same as in bootsect.s;

INITSEG  = 0x9000	; we move boot here - out of the way
SYSSEG   = 0x1000	; system loaded at 0x10000 (65536).
SETUPSEG = 0x9020	; this is the current segment

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

entry start
start:

1.获取系统信息
2.移动system块位置
3.加载gdt和idt
4.进入保护模式

### 获取系统信息，以0x90000作为起始位置，逐个存入 ### 
; ok, the read went well so we get current cursor position and save it for
; posterity.

	mov	ax,#INITSEG	; this is done in bootsect already, but...
	mov	ds,ax
	
	mov	ah,#0x03	# ah指定要执行的子功能代码（中断服务），这里是3，表示获取光标位置; read cursor pos

	xor	bh,bh       # bh用于指定获取光标的显示页面号
					# 这里置零，表示获取当前活动页面上的光标位置

	int	0x10		# BIOS的显示服务中断程序

	mov	[0],dx		# dx中存放了结果，dh表示行号，dl表示列号
					# 将结果存放在内存地址为0的变量中
					# [0]是偏移地址，再加上基址0x90000，那么光标位置就被存储在了0x90000的位置
					# 之后在初始化控制台的时候用到

; Get memory size (extended mem, kB)

	mov	ah,#0x88	# 获取系统扩展内存大小子功能，最大是64MB
	int	0x15		# 各种系统管理和控制功能
	mov	[2],ax

				 	# 获取显卡显示模式
; Get video-card data: 

	mov	ah,#0x0f
	int	0x10
	mov	[4],bx		; bh = display page
	mov	[6],ax		; al = video mode, ah = window width

; check for EGA/VGA and some config parameters

	mov	ah,#0x12
	mov	bl,#0x10
	int	0x10
	mov	[8],ax
	mov	[10],bx
	mov	[12],cx

					# 获取第一块硬盘信息
; Get hd0 data

	mov	ax,#0x0000
	mov	ds,ax
	lds	si,[4*0x41]
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0080
	mov	cx,#0x10
	rep
	movsb

					# 获取第二块硬盘信息
; Get hd1 data

	mov	ax,#0x0000
	mov	ds,ax
	lds	si,[4*0x46]
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0090
	mov	cx,#0x10
	rep
	movsb

; Check that there IS a hd1 :-)

	mov	ax,#0x01500
	mov	dl,#0x81
	int	0x13
	jc	no_disk1
	cmp	ah,#3
	je	is_disk1
no_disk1:
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0090
	mov	cx,#0x10
	mov	ax,#0x00
	rep
	stosb
is_disk1:


### 准备进入保护模式 ###
; now we want to move to protected mode ...

				### 关闭中断 
				# 因为新移动的system块覆盖了BIOS写好的中断向量表，因此停止产生中断
				# 后续会添加自己的中断向量表
	cli			; no interrupts allowed ;



				### 移动system块的位置 
				# 每次移动0x10000大小，即64k，循环8次，共移动512KB的数据
				# 因此，最终system可用空间为块所在位置为0x00000-0x80000
; first we move the system to it's rightful place
	mov	ax,#0x0000  
	cld			; 'direction'=0, movs moves forward							
do_move:
	mov	es,ax		; destination segment	# es为目标位置，为0x00000
	add	ax,#0x1000							# system块原起始位置在0x10000
	cmp	ax,#0x9000  						# 如果ax已经是0x80000，则表示已经完成移动
	jz	end_move
	mov	ds,ax		; source segment   		# ds为源位置
	sub	di,di								
	sub	si,si
	mov 	cx,#0x8000						# cs为复制次数，要移动64k字节
											
	rep
	movsw									# ds：si到es：di的赋值过程
	jmp	do_move


				
; then we load the segment descriptors

end_move:
	mov	ax,#SETUPSEG	; right, forgot this at first. didn't work :-)
	mov	ds,ax

				### 将中断描述符表和全局描述符表加载到CPU中的idtr和gdtr
	lidt	idt_48		; load idt with 0,0
	lgdt	gdt_48		; load gdt with whatever appropriate




; that was painless, now we enable A20

	call	empty_8042
	mov	al,#0xD1		; command write
	out	#0x64,al
	call	empty_8042
	mov	al,#0xDF		; A20 on
	out	#0x60,al
	call	empty_8042

; well, that went ok, I hope. Now we have to reprogram the interrupts :-(
; we put them right after the intel-reserved hardware interrupts, at
; int 0x20-0x2F. There they won't mess up anything. Sadly IBM really
; messed this up with the original PC, and they haven't been able to
; rectify it afterwards. Thus the bios puts interrupts at 0x08-0x0f,
; which is used for the internal hardware interrupts as well. We just
; have to reprogram the 8259's, and it isn't fun.

	mov	al,#0x11		; initialization sequence
	out	#0x20,al		; send it to 8259A-1
	.word	0x00eb,0x00eb		; jmp $+2, jmp $+2
	out	#0xA0,al		; and to 8259A-2
	.word	0x00eb,0x00eb
	mov	al,#0x20		; start of hardware int's (0x20)
	out	#0x21,al
	.word	0x00eb,0x00eb
	mov	al,#0x28		; start of hardware int's 2 (0x28)
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0x04		; 8259-1 is master
	out	#0x21,al
	.word	0x00eb,0x00eb
	mov	al,#0x02		; 8259-2 is slave
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0x01		; 8086 mode for both
	out	#0x21,al
	.word	0x00eb,0x00eb
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0xFF		; mask off all interrupts for now
	out	#0x21,al
	.word	0x00eb,0x00eb
	out	#0xA1,al

; well, that certainly wasn't fun :-(. Hopefully it works, and we don't
; need no steenking BIOS anyway (except for the initial loading :-).
; The BIOS-routine wants lots of unnecessary data, and it's less
; "interesting" anyway. This is how REAL programmers do it.
;
; Well, now's the time to actually move into protected mode. To make
; things as simple as possible, we do no register set-up or anything,
; we let the gnu-compiled 32-bit programs do that. We just jump to
; absolute address 0x00000, in 32-bit protected mode.


				### 开启保护模式
				# lmsw（load Machine Status Word）指令将cr0寄存器的最低位置为1，开启保护模式
	mov	ax,#0x0001	; protected mode (PE) bit
	lmsw	ax		; This is it;
				# 8在段选择子结构中，索引为1，即指向gdt中的代码段描述符（cs）
				# cs的基址为0，偏移又为0，所以这里跳转到了system块所在地址0x00000
	jmpi	0,8		; jmp offset 0 of segment 8 (cs)


; This routine checks that the keyboard command queue is empty
; No timeout is used - if this hangs there is something wrong with
; the machine, and we probably couldn't proceed anyway.
empty_8042:
	.word	0x00eb,0x00eb
	in	al,#0x64	; 8042 status port
	test	al,#2		; is input buffer full?
	jnz	empty_8042	; yes - loop
	ret


				# 全局描述符表（gdt）内容定义	
				# 第二个和第三个段描述符的段基址都是0
				# 即物理地址直接等于程序员给出的逻辑地址（准确说是逻辑地址中的偏移地址）
gdt:
				# 第一个为空
	.word	0,0,0,0		; dummy
				# 代码段描述符
	.word	0x07FF		; 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		; base address=0
	.word	0x9A00		; code read/exec
	.word	0x00C0		; granularity=4096, 386
				# 数据段描述符
	.word	0x07FF		; 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		; base address=0
	.word	0x9200		; data read/write
	.word	0x00C0		; granularity=4096, 386

idt_48:
	.word	0			; idt limit=0
	.word	0,0			; idt base=0L


				### 全局描述符表（gdt）定义
				# 其被加载到CPU的gdtr寄存器中
gdt_48:
				# 低16为gdt可以存放的项目数量，一项占用8字节，总共可放256项
	.word	0x800		; gdt limit=2048, 256 GDT entries
				# 高32位为gdt在内存中的起始位置
				# gdt表示一个符号，在编译时确定位置，地址为：0x90000，0x200+gdt，也就是0x90200+gdt
	.word	512+gdt,0x9	; gdt base = 0X9xxxx
	
.text
endtext:
.data
enddata:
.bss
endbss:
