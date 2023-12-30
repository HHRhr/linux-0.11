;
; SYS_SIZE is the number of clicks (16 bytes) to be loaded.
; 0x3000 is 0x30000 bytes = 196kB, more than enough for current
; versions of linux
;
SYSSIZE = 0x3000
;
;	bootsect.s		(C) 1991 Linus Torvalds
;
; bootsect.s is loaded at 0x7c00 by the bios-startup routines, and moves
; iself out of the way to address 0x90000, and jumps there.
;
; It then loads 'setup' directly after itself (0x90200), and the system
; at 0x10000, using BIOS interrupts. 
;
; NOTE; currently system is at most 8*65536 bytes long. This should be no
; problem, even in the future. I want to keep it simple. This 512 kB
; kernel size should be enough, especially as this doesn't contain the
; buffer cache as in minix
;
; The loader has been made as simple as possible, and continuos
; read errors will result in a unbreakable loop. Reboot by hand. It
; loads pretty fast by getting whole sectors at a time whenever possible.

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

SETUPLEN = 4				; nr of setup-sectors
BOOTSEG  = 0x07c0			; original address of boot-sector
INITSEG  = 0x9000			; we move boot here - out of the way
SETUPSEG = 0x9020			; setup starts here
SYSSEG   = 0x1000			; system loaded at 0x10000 (65536).
ENDSEG   = SYSSEG + SYSSIZE		; where to stop loading

; ROOT_DEV:	0x000 - same type of floppy as boot.
;		0x301 - first partition on first drive etc
ROOT_DEV = 0x306


1.移动启动区位置
2.设定ds，cs，sp的值
3.加载setup块
4.加载system块

### 加载启动区 ###

entry start
start:
						 ### 启动区到初始化区的复制
	mov	ax,#BOOTSEG      
	mov	ds,ax            # 将0x07c0存入ds中作为基址，其左移四位为0x7c00，即启动区的起始地址
						 
						 
	mov	ax,#INITSEG		 
	mov	es,ax
	mov	cx,#256			 # 计数寄存器
	sub	si,si            # si寄存器清零
	sub	di,di
	rep	movw			 # 将ds（0x7c00）处的值复制到es（0x9000）处，每次复制一个词大小（16位），复制cx（256）次
						 # 源基址：源变址 ds：si  目的基址：目的变址es：di
						 # 效果就是将启动区整体复制到了0x9000处
						
						 ### 跳转到go指定的代码段并开始执行
	jmpi	go,INITSEG   # 段间跳转指令，跳转至INITSEG：go位置
						 # 这里基址为0x9000，左移四位变成0x90000，偏移地址为go
						 # go是一段代码的标签，值未知，在被编译为机器码时才确定，值为这段代码所在文件的偏移地址

                         
						 ### go标签所在位置，也是代码段的开始，地址为cs：ip
						 # 上个跳转命令给cs赋值0x9000，给ip赋值go标签偏移地址
go:	mov	ax,cs            # 接下来给其他段寄存器也赋值0x9000							 
	mov	ds,ax			 # ds保持指向整体基址
	mov	es,ax
; put stack at 0x9ff00.
	mov	ss,ax			 # 与下面的sp配合，计算栈顶地址，即#0x9FF00
						 # 栈向下发展，将栈顶设置为远大于512的数，确保和启动区保持足够距离
	mov	sp,#0xFF00		; arbitrary value >>512

### 简单说，上述过程就是设置了如何访问数据的数据段，如何访问代码的代码段，以及如何访问栈的栈顶指针，
### 也即初步做了一次内存规划，从 CPU 的角度看，访问内存，就这么三块地方而已。



### 加载setup.s ###

; load the setup-sectors directly after the bootblock.
; Note that 'es' is already set up.

						 ### 通过中断，将启动扇区后的四个扇区加载到内存中
						 ### 这四个扇区属于setup扇区，起始位置放在0x90200
load_setup:
	mov	dx,#0x0000		; drive 0, head 0     # 驱动器0，磁头0
	mov	cx,#0x0002		; sector 2, track 0   # 磁道0，扇区2
	mov	bx,#0x0200		; address = 512, in INITSEG   # 目的存放地址，即偏移地址，基址是0x90000
	mov	ax,#0x0200+SETUPLEN	; service 2, nr of sectors  # 服务2，读取数量4
	int	0x13			; read it             # BISO提供的中断，上面四个寄存器是其参数。用于读取磁盘
	jnc	ok_load_setup		; ok - continue

	mov	dx,#0x0000
	mov	ax,#0x0000		; reset the diskette
	int	0x13
	j	load_setup


### 加载system块 ###
ok_load_setup:

; Get disk drive parameters, specifically nr of sectors/track

	mov	dl,#0x00
	mov	ax,#0x0800		; AH=8 is get drive parameters
	int	0x13
	mov	ch,#0x00
	seg cs
	mov	sectors,cx
	mov	ax,#INITSEG
	mov	es,ax

; Print some inane message

	mov	ah,#0x03		; read cursor pos
	xor	bh,bh
	int	0x10
	
	mov	cx,#24
	mov	bx,#0x0007		; page 0, attribute 7 (normal)
	mov	bp,#msg1
	mov	ax,#0x1301		; write string, move cursor
	int	0x10

						 ### 将其后的240个扇区加载到内存中，这里称为system块
; ok, we've written the message, now
; we want to load the system (at 0x10000)

	mov	ax,#SYSSEG
	mov	es,ax		; segment of 0x010000   # system块的起始地址为0x10000
	call	read_it
	call	kill_motor

; After that we check which root-device to use. If the device is
; defined (!= 0), nothing is done and the given device is used.
; Otherwise, either /dev/PS0 (2,28) or /dev/at0 (2,8), depending
; on the number of sectors that the BIOS reports currently.

	seg cs
	mov	ax,root_dev
	cmp	ax,#0
	jne	root_defined
	seg cs
	mov	bx,sectors
	mov	ax,#0x0208		; /dev/ps0 - 1.2Mb
	cmp	bx,#15
	je	root_defined
	mov	ax,#0x021c		; /dev/PS0 - 1.44Mb
	cmp	bx,#18
	je	root_defined
undef_root:
	jmp undef_root
root_defined:
	seg cs
	mov	root_dev,ax

; after that (everyting loaded), we jump to
; the setup-routine loaded directly after
; the bootblock:

	jmpi	0,SETUPSEG	 ### 跳转至0x90200，setup块起始位置





; This routine loads the system at address 0x10000, making sure
; no 64kB boundaries are crossed. We try to load it as fast as
; possible, loading whole tracks whenever we can.
;
; in:	es - starting address segment (normally 0x1000)
;
sread:	.word 1+SETUPLEN	; sectors read of current track
head:	.word 0			; current head
track:	.word 0			; current track

read_it:
	mov ax,es
	test ax,#0x0fff
die:	jne die			; es must be at 64kB boundary
	xor bx,bx		; bx is starting address within segment
rp_read:
	mov ax,es
	cmp ax,#ENDSEG		; have we loaded all yet?
	jb ok1_read
	ret
ok1_read:
	seg cs
	mov ax,sectors
	sub ax,sread
	mov cx,ax
	shl cx,#9
	add cx,bx
	jnc ok2_read
	je ok2_read
	xor ax,ax
	sub ax,bx
	shr ax,#9
ok2_read:
	call read_track
	mov cx,ax
	add ax,sread
	seg cs
	cmp ax,sectors
	jne ok3_read
	mov ax,#1
	sub ax,head
	jne ok4_read
	inc track
ok4_read:
	mov head,ax
	xor ax,ax
ok3_read:
	mov sread,ax
	shl cx,#9
	add bx,cx
	jnc rp_read
	mov ax,es
	add ax,#0x1000
	mov es,ax
	xor bx,bx
	jmp rp_read

read_track:
	push ax
	push bx
	push cx
	push dx
	mov dx,track
	mov cx,sread
	inc cx
	mov ch,dl
	mov dx,head
	mov dh,dl
	mov dl,#0
	and dx,#0x0100
	mov ah,#2
	int 0x13
	jc bad_rt
	pop dx
	pop cx
	pop bx
	pop ax
	ret
bad_rt:	mov ax,#0
	mov dx,#0
	int 0x13
	pop dx
	pop cx
	pop bx
	pop ax
	jmp read_track

/*
 * This procedure turns off the floppy drive motor, so
 * that we enter the kernel in a known state, and
 * don't have to worry about it later.
 */
kill_motor:
	push dx
	mov dx,#0x3f2
	mov al,#0
	outb
	pop dx
	ret

sectors:
	.word 0

msg1:
	.byte 13,10
	.ascii "Loading system ..."
	.byte 13,10,13,10

.org 508
root_dev:
	.word ROOT_DEV
boot_flag:
	.word 0xAA55

.text
endtext:
.data
enddata:
.bss
endbss:
