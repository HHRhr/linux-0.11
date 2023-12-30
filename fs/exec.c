/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <a.out.h>

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/segment.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES 32

/*
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value.
 */


					# 为新程序构建参数表
					# 这里先为环境变量和命令行参数空出了指针位置，然后再倒序进行填充
static unsigned long * create_tables(char * p,int argc,int envc)
{
	unsigned long *argv,*envp;
	unsigned long * sp;

	sp = (unsigned long *) (0xfffffffc & (unsigned long) p);
	sp -= envc+1;
	envp = sp;
	sp -= argc+1;
	argv = sp;

					# 将envp存入指定位置，fs作为基址，sp作为偏移地址
	put_fs_long((unsigned long)envp,--sp);
	put_fs_long((unsigned long)argv,--sp);
	put_fs_long((unsigned long)argc,--sp);

					# 至此，参数表中从上至下依次是：

						HOME=/			envc个envp参数
						/bin/bash		argc个argv参数							p的初始指向
						NULL								共envc+1个空项
						NULL			envp指向	
						NULL								共argc+1个空项
						NULL			argv指向									
						envp指针
						argv指针
						argc


					# 接着，填充之前的空项，指向上面的具体参数，在此期间，p指针持续上移
					
					# 将命令行各参数指针放入前面空出来的相应地方，最后放置一个NULL指针
	while (argc-->0) {
		put_fs_long((unsigned long) p,argv++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,argv);

					# 环境变量各指针放入前面空出来的相应地方，最后放置一个NULL指针
	while (envc-->0) {
		put_fs_long((unsigned long) p,envp++);
		while (get_fs_byte(p++)) /* nothing */ ;
	}
	put_fs_long(0,envp);

	return sp;		# 返回最低处指针，作为初始的栈顶
}

/*
 * count() counts the number of arguments/envelopes
 */
static int count(char ** argv)
{
	int i=0;
	char ** tmp;

	if (tmp = argv)
		while (get_fs_long((unsigned long *) (tmp++)))
			i++;

	return i;
}

/*
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 * 
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 * 
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 */
static unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
		unsigned long p, int from_kmem)
{
	char *tmp, *pag;
	int len, offset = 0;
	unsigned long old_fs, new_fs;

	if (!p)
		return 0;	/* bullet-proofing */
	new_fs = get_ds();
	old_fs = get_fs();
	if (from_kmem==2)
		set_fs(new_fs);
	while (argc-- > 0) {
		if (from_kmem == 1)
			set_fs(new_fs);
		if (!(tmp = (char *)get_fs_long(((unsigned long *)argv)+argc)))
			panic("argc is wrong");
		if (from_kmem == 1)
			set_fs(old_fs);
		len=0;		/* remember zero-padding */
		do {
			len++;
		} while (get_fs_byte(tmp++));
		if (p-len < 0) {	/* this shouldn't happen - 128kB */
			set_fs(old_fs);
			return 0;
		}
		while (len) {
			--p; --tmp; --len;
			if (--offset < 0) {
				offset = p % PAGE_SIZE;
				if (from_kmem==2)
					set_fs(old_fs);
				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
				    !(pag = (char *) page[p/PAGE_SIZE] =
				      (unsigned long *) get_free_page())) 
					return 0;
				if (from_kmem==2)
					set_fs(new_fs);

			}
			*(pag + offset) = get_fs_byte(tmp);
		}
	}
	if (from_kmem==2)
		set_fs(old_fs);
	return p;
}


					
					# 修改ldt中的段限长
					# 将参数表中已存放数据的页面映射到物理内存中数据段的后面
static unsigned long change_ldt(unsigned long text_size,unsigned long * page)
{
	unsigned long code_limit,data_limit,code_base,data_base;
	int i;

					# 设置新的代码段限长code_limit和数据段限长64M

					# 这里想让代码段限长为4K的整数倍，所以先加了PAGE_SIZE，再4K对齐
	code_limit = text_size+PAGE_SIZE -1;
	code_limit &= 0xFFFFF000;
	data_limit = 0x4000000;
	code_base = get_base(current->ldt[1]);
	data_base = code_base;
	set_base(current->ldt[1],code_base);
	set_limit(current->ldt[1],code_limit);
	set_base(current->ldt[2],data_base);
	set_limit(current->ldt[2],data_limit);
/* make sure fs points to the NEW data segment */
	__asm__("pushl $0x17\n\tpop %%fs"::);


	data_base += data_limit;	# 指向64M空间的最末端
	for (i=MAX_ARG_PAGES-1 ; i>=0 ; i--) {
		data_base -= PAGE_SIZE;
		if (page[i])
					
					# put page将物理内存页面和线性地址进行映射，即将页面的地址存放到线性地址对应的页表中
					# 因此参数表在逻辑上，位于线性空间中的末端128K
			put_page(page[i],data_base);		
	}
	return data_limit;
}


					# execve系统调用函数的具体实现
					# 用于加载并执行一个进程
/*
 * 'do_execve()' executes a new program.
 */
int do_execve(unsigned long * eip,			# 指向栈中的eip位置
						long tmp,			
						char * filename,	# 要执行的程序路径  /bin/sh
						char ** argv, 		# { "/bin/sh", NULL }
						char ** envp)		# { "HOME=/", NULL }
{
	struct m_inode * inode;
	struct buffer_head * bh;
	struct exec ex;


						# 参数表页面指针数组
						# 指向实际的物理页面
	unsigned long page[MAX_ARG_PAGES];
	
	
	int i,argc,envc;
	int e_uid, e_gid;
	int retval;
	int sh_bang = 0;


						# 参数表起始位置，每个进程的参数表大小为128K，位于线性地址空间的最末端
						# p = 4096 * 32 - 4 = 0x20000 - 4 = 128K - 4 =  0x1FFFC
						# 此时p指向末端位置向下的4字节偏移位置
	unsigned long p=PAGE_SIZE*MAX_ARG_PAGES-4;


	if ((0xffff & eip[1]) != 0x000f)
		panic("execve called from supervisor mode");
	for (i=0 ; i<MAX_ARG_PAGES ; i++)	/* clear page-table */
		page[i]=0;

						### 取inode
						# 根据给出的程序路径，找到其inode
	if (!(inode=namei(filename)))		/* get executables inode */
		return -ENOENT;
	argc = count(argv);
	envc = count(envp);
	

restart_interp:
	if (!S_ISREG(inode->i_mode)) {	/* must be regular file */
		retval = -EACCES;
		goto exec_error2;
	}

						# 检查被执行文件的权限
	i = inode->i_mode;
	e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
	e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;
	if (current->euid == inode->i_uid)
		i >>= 6;
	else if (current->egid == inode->i_gid)
		i >>= 3;
	if (!(i & 1) &&
	    !((inode->i_mode & 0111) && suser())) {
		retval = -ENOEXEC;
		goto exec_error2;
	}

						### 读取程序数据
						# 根据inode读取第一个数据块(1KB)
	if (!(bh = bread(inode->i_dev,inode->i_zone[0]))) {
		retval = -EACCES;
		goto exec_error2;
	}

						### 解析程序的头部信息
						# 取数据，解析成exec结构体(执行头)，其中存储了可执行程序的相关属性
						# exec结构体定义在/include/a.out.h，其中包含很多与可执行文件相关的数据结构和常量定义，即a.out的格式信息
						# a.out是旧版Linux中可执行程序的格式，现代的Linux使用ELF格式
	ex = *((struct exec *) bh->b_data);	/* read exec-header */


						### 判断是脚本文件还是可执行程序

						### 如果是脚本文件
	if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang)) {
		/*
		 * This section does the #! interpretation.
		 * Sorta complicated, but hopefully it will work.  -TYT
		 */

		char buf[1023], *cp, *interp, *i_name, *i_arg;
		unsigned long old_fs;

		strncpy(buf, bh->b_data+2, 1022);
		brelse(bh);
		iput(inode);
		buf[1022] = '\0';
		if (cp = strchr(buf, '\n')) {
			*cp = '\0';
			for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++);
		}
		if (!cp || *cp == '\0') {
			retval = -ENOEXEC; /* No interpreter name found */
			goto exec_error1;
		}
		interp = i_name = cp;
		i_arg = 0;
		for ( ; *cp && (*cp != ' ') && (*cp != '\t'); cp++) {
 			if (*cp == '/')
				i_name = cp+1;
		}
		if (*cp) {
			*cp++ = '\0';
			i_arg = cp;
		}
		/*
		 * OK, we've parsed out the interpreter name and
		 * (optional) argument.
		 */
		if (sh_bang++ == 0) {
			p = copy_strings(envc, envp, page, p, 0);
			p = copy_strings(--argc, argv+1, page, p, 0);
		}
		/*
		 * Splice in (1) the interpreter's name for argv[0]
		 *           (2) (optional) argument to interpreter
		 *           (3) filename of shell script
		 *
		 * This is done in reverse order, because of how the
		 * user environment and arguments are stored.
		 */
		p = copy_strings(1, &filename, page, p, 1);
		argc++;
		if (i_arg) {
			p = copy_strings(1, &i_arg, page, p, 2);
			argc++;
		}
		p = copy_strings(1, &i_name, page, p, 2);
		argc++;
		if (!p) {
			retval = -ENOMEM;
			goto exec_error1;
		}
		/*
		 * OK, now restart the process with the interpreter's inode.
		 */
		old_fs = get_fs();
		set_fs(get_ds());
		if (!(inode=namei(interp))) { /* get executables inode */
			set_fs(old_fs);
			retval = -ENOENT;
			goto exec_error1;
		}
		set_fs(old_fs);
		goto restart_interp;
	}

						### 如果是二进制文件
						
	brelse(bh);			# 释放缓冲区
						# 前面已将数据复制给ex变量，存储在栈中


	if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
		ex.a_text+ex.a_data+ex.a_bss>0x3000000 ||
		inode->i_size < ex.a_text+ex.a_data+ex.a_syms+N_TXTOFF(ex)) {
		retval = -ENOEXEC;
		goto exec_error2;
	}
	if (N_TXTOFF(ex) != BLOCK_SIZE) {
		printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
		retval = -ENOEXEC;
		goto exec_error2;
	}


						### 向参数表中填充环境变量和命令行参数
						# 先写入envp:HOME=/，p指针下移7字节
						# 再写入argv:/bin/sh，p指针下移8字节
						# 这里申请了一页物理内存，地址存放在page数组中， 表示参数表的这一页存放了数据
	if (!sh_bang) {
		p = copy_strings(envc,envp,page,p,0);
		p = copy_strings(argc,argv,page,p,0);
		if (!p) {
			retval = -ENOMEM;
			goto exec_error2;
		}
	}



/* OK, This is the point of no return */
	if (current->executable)
		iput(current->executable);

					# 程序文件块即为此inode块
	current->executable = inode;		
	for (i=0 ; i<32 ; i++)
		current->sigaction[i].sa_handler = NULL;
	for (i=0 ; i<NR_OPEN ; i++)
		if ((current->close_on_exec>>i)&1)
			sys_close(i);
	current->close_on_exec = 0;
					# 释放掉复制于进程1的页目录项和页表项,所以可以通过修改eip到新程序的入口点，引起缺页中断
	free_page_tables(get_base(current->ldt[1]),get_limit(0x0f));
	free_page_tables(get_base(current->ldt[2]),get_limit(0x17));
	if (last_task_used_math == current)
		last_task_used_math = NULL;
	current->used_math = 0;


					### 更新ldt和p指针
					# change_ldt修改代码段限长code_limit，a_text是代码段长度
					# 函数返回值是数据段限长，这里是64M

					# 更新p值，原来的p值为128K-19，现在的p值为64M-19，下面构建参数表需要直接使用p作为偏移量
	p += change_ldt(ex.a_text,page)-MAX_ARG_PAGES*PAGE_SIZE;
	
	
					### 构建参数表
	p = (unsigned long) create_tables((char *)p,argc,envc);



	current->brk = ex.a_bss +
		(current->end_data = ex.a_data +
		(current->end_code = ex.a_text));
					# 设置进程的堆栈开始位置为p指针所在页面起始位置
	current->start_stack = p & 0xfffff000;
	current->euid = e_uid;
	current->egid = e_gid;

					# 清除最后一页上的空闲空间，用于存放bss
	i = ex.a_text+ex.a_data;
	while (i&0xfff)
		put_fs_byte(0,(char *) (i++));


					### 修正eip和esp指针
					# 参数eip是传入参数，指向栈中的eip位置
					# eip[0]指向栈中的eip
					# eip[3]指向栈中的esp

					# a_entry是新程序的入口地址，这里将eip指向这个入口地址
	eip[0] = ex.a_entry;		/* eip, magic happens :-) */
					# 设置esp的值为p，即指向新的栈顶
	eip[3] = p;			/* stack pointer */

					# 调用结束后，eip指向了代码执行入口，esp指向了栈顶
					# 在中断恢复后，栈中的值会被传给相应寄存器，达到更换执行程序的目的
	
	return 0;

exec_error2:
	iput(inode);
exec_error1:
	for (i=0 ; i<MAX_ARG_PAGES ; i++)
		free_page(page[i]);
	return(retval);
}
