/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

					# 取当前进程的ldt中的段限长，即父子段限长是一致的
	code_limit=get_limit(0x0f);				# 进程0的段限长是640K，进程1的段限长也是640K
	data_limit=get_limit(0x17);
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");

	new_data_base = new_code_base = nr * 0x4000000;		# 相当于每个进程在线性空间中占据64M空间
	p->start_code = new_code_base;

	set_base(p->ldt[1],new_code_base);		# 设置ldt表项，指向线性空间
	set_base(p->ldt[2],new_data_base);		# 至此，通过分段机制，将不同进程映射到了互不干扰的线性空间中
											# 64M空间的顶端存放最多128K的环境参数数据块，接着存放堆栈段
											# 64M空间的低端依次存放code、date、bss
											# bss(未初始化的数据段)，是进程中声明为静态/全局但未初始化的变量所占内存区域
				

				# 复制页表
				# 至此，通过分页机制，将两个进程从不同的线性空间映射到相同的物理空间
				# 即父子进程共享相同的物理内存页

				# 在copy_page_tables函数中，父子进程的页表项都被置为只读，
				# 因此在读内存时不会出现问题，但需要写内存时，则会发生缺页中断，这时再分配新的内存页并复制原页
				# 以上即写时复制
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;

					# 从mem_map数组中寻找空闲页，在位图中置为1，表示占用，返回该页的内存地址
					# 将进程块指针指向该页，由此一个新进程在内存中占据了一块空间
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;

	task[nr] = p;	# 归入task数组管理
	*p = *current;	# 将当前进程的全部内容复制给新进程/* NOTE! this doesn't copy the supervisor stack */

					# 一些值需要进行个性化修改，包括进程的元信息、父子关系，还有tss的内容
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = last_pid;
	p->father = current->pid;
	p->counter = p->priority;
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;
	p->tss.back_link = 0;

						# 设置内核态时，ss和esp的指向
						# esp指向该进程结构体所在内存页的最顶端
						# ss指向gdt中的数据段，即栈基址为0，偏移为esp
						# 由于从内核态返回时，CPU中esp的值不会被保存至tss.esp0中，因此每次进程进入内核态时，栈内容都是空的
	p->tss.esp0 = PAGE_SIZE + (long) p;		
	p->tss.ss0 = 0x10;

	p->tss.eip = eip;
	p->tss.eflags = eflags;
	p->tss.eax = 0;
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;	# 等于父进程的esp，即偏移地址是一样的，那么在缺页中断之前，指向的位置也是一样的
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;

	p->tss.ldt = _LDT(nr);		# _LDT宏通过 ((((unsigned long) n)<<4)+(FIRST_LDT_ENTRY<<3)) 计算得出
								# 由于每个表项占8字节，所以是左移3位
								# 因此_LDT宏计算出的是该ldt(n)在gdt中的字节偏移量
								# tss通过此字段来自动加载ldtr寄存器，将其指向gtd中的ldt
	
								
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));

					# 复制页表
					# 如果是复制于进程0(内核)，则复制160个页表项，即进程1与进程0共享内核这640K的物理内存
					# 进程1如果操作了堆栈，则发生写时复制，则会复制进程0的堆栈(即user_stack，4K大小)到一个新的内存页
					# 此时进程1拥有了一个属于自己的堆栈
					
					# 如果是复制于其他进程，则复制1024个页面(即一个页表大小)，即子进程与父进程共享4M的物理内存
					# 同理，操作堆栈会发生写时复制，子进程将拥有自己的堆栈
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}

	for (i=0; i<NR_OPEN;i++)	# 已打开文件的引用数+1
		if (f=p->filp[i])
			f->f_count++;		

	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
					
					# 设置gdt中tss(1)和ldt(1)指向进程块
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;
}

int find_empty_process(void)
{
	int i;

	repeat:
					# 如果pid加1后超过long的最大值，变为0，则置1后重新找pid
		if ((++last_pid)<0) last_pid=1;

		for(i=0 ; i<NR_TASKS ; i++)
					# 有使用这个pid的进程，换下一个pid重新试
			if (task[i] && task[i]->pid == last_pid) goto repeat;	

					# 找到一个空位，返回索引i
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
