				
				# 特权级:
				# 当前代码段选择子中有CPL(当前特权级)，要跳转的目标段选择子中有RPL(请求特权级)，
				#   目标段描述符中有DPL(目标特权级)，需要CPL=DPL才可跳转，即用户态向用户态跳转，内核态向内核态跳转
				# 特权级保护属于CPU保护机制中的段保护机制
				# 代码跳转只能同特权级，数据访问只能高特权级访问低特权级
				
				# 特权级转换：操作系统中经常存在特权级的转换：
				# 	处于用户态的程序，通过触发中断，可以进入内核态，之后再通过中断返回，又可以恢复为用户态

				# 开启用户模式
				# 当前处于内核态，为跳转至用户态，需要利用中断返回的方式
				# 先将值压入栈中，等中断返回时，相关硬件会将值弹出，并赋值给相应的寄存器
				
				# 在sched.c中已经设置了tr和ldtr，均指向进程0的tss和ldt，因此在中断返回后，即是开始执行进程0

#define move_to_user_mode() \
__asm__ ("movl %%esp,%%eax\n\t" \	
									# 用户栈位置
	"pushl $0x17\n\t" \				# 堆栈段ss	0x00010111，CPL为3，表示特权级为用户态，TI为1，表示从ldt中读取
	"pushl %%eax\n\t" \				# 栈指针sp	指向user_stack中的原位置，即进程0在用户态时继续使用内核代码的堆栈

	"pushfl\n\t" \					# 标志寄存器eflags

									# 用户代码位置
	"pushl $0x0f\n\t" \				# 代码段cs	0x00001111，CPL为3，表示特权级为用户态，TI为1，表示从ldt中读取

	"pushl $1f\n\t" \				# 代码指针ip，将下面标号为1的指令的偏移地址压入栈中
	
	"iret\n" \						# 中断返回指令，跳转到下面开始执行
	
	"1:\tmovl $0x17,%%eax\n\t" \	# 初始化剩余段选择器，且均从ldt中读取，段偏移为2，即指向ldt中的数据段
	"movw %%ax,%%ds\n\t" \
	"movw %%ax,%%es\n\t" \
	"movw %%ax,%%fs\n\t" \
	"movw %%ax,%%gs" \
	:::"ax")

#define sti() __asm__ ("sti"::)
#define cli() __asm__ ("cli"::)
#define nop() __asm__ ("nop"::)

#define iret() __asm__ ("iret"::)

					# 向idt中写入中断
#define _set_gate(gate_addr,type,dpl,addr) \
__asm__ ("movw %%dx,%%ax\n\t" \
	"movw %0,%%dx\n\t" \
	"movl %%eax,%1\n\t" \
	"movl %%edx,%2" \
	: \
	: "i" ((short) (0x8000+(dpl<<13)+(type<<8))), \
	"o" (*((char *) (gate_addr))), \
	"o" (*(4+(char *) (gate_addr))), \
	"d" ((char *) (addr)),"a" (0x00080000))

#define set_intr_gate(n,addr) \
	_set_gate(&idt[n],14,0,addr)


					# 发现它们均指向_set_gate
					# trap的特权级是0，代表内核态
					# system的特权级是3，代表用户态
#define set_trap_gate(n,addr) \
	_set_gate(&idt[n],15,0,addr)

#define set_system_gate(n,addr) \
	_set_gate(&idt[n],15,3,addr)


#define _set_seg_desc(gate_addr,type,dpl,base,limit) {\
	*(gate_addr) = ((base) & 0xff000000) | \
		(((base) & 0x00ff0000)>>16) | \
		((limit) & 0xf0000) | \
		((dpl)<<13) | \
		(0x00408000) | \
		((type)<<8); \
	*((gate_addr)+1) = (((base) & 0x0000ffff)<<16) | \
		((limit) & 0x0ffff); }

#define _set_tssldt_desc(n,addr,type) \
__asm__ ("movw $104,%1\n\t" \
	"movw %%ax,%2\n\t" \
	"rorl $16,%%eax\n\t" \
	"movb %%al,%3\n\t" \
	"movb $" type ",%4\n\t" \
	"movb $0x00,%5\n\t" \
	"movb %%ah,%6\n\t" \
	"rorl $16,%%eax" \
	::"a" (addr), "m" (*(n)), "m" (*(n+2)), "m" (*(n+4)), \
	 "m" (*(n+5)), "m" (*(n+6)), "m" (*(n+7)) \
	)

#define set_tss_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x89")
#define set_ldt_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x82")
