/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */
					# 从CMOS存储器中读取时间
#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \		# 0x70是写端口号,这里向该端口传送要读取的地址:0x80|addr
inb_p(0x71); \					# 0x71是读端口号,从该端口获得想要的数据
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
					# 计算从1970到当前时间的秒数,作为当前系统时间
					# 因此即使获取了当前时间,也要将其转换为UNIX时间戳形式,便于比较.因为CMOS的读取较为耗时,不能频繁读
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;

struct drive_info { char dummy[32]; } drive_info;



void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
					### 各参数设定
 	ROOT_DEV = ORIG_ROOT_DEV;
					# setup块中通过BIOS获取了一些系统信息并存放在0x90000的位置
					# 这里取出了放在0x90080处的硬盘参数表信息
 	drive_info = DRIVE_INFO;

					# 设定缓冲区界限和主内存界限，且 buffer_memory_end = main_memory_start
					# 缓冲区界限  0x80000 -> buffer_memory_end，对缓冲取的管理在buffer_init()中
					# 主内存界限  main_memory_start -> memory_end，对主内存区的管理在mem_init()中
	memory_end = (1<<20) + (EXT_MEM_K<<10);		# 1*1024*1024 + 扩展内存大小*1024，单位是字节B
	memory_end &= 0xfffff000;					# 忽略不到4KB的内存
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	main_memory_start = buffer_memory_end;

#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif

					### 各模块初始化 
	mem_init(main_memory_start,memory_end);		 # /mm/memory.c
	trap_init();	# 设置中断，填充idt表			# /kernel/traps.c
	blk_dev_init();	# 硬盘初始化					# /kernel/blk_drv/ll_rw_blk.c
	chr_dev_init();	# 为空,和tty_init()一回事
	tty_init();		# 终端初始化，控制台相关中断（包括键盘中断）在这里被设置  # /kernel/chr_drv/tty_io.c
	time_init();	# 设置系统时间startup_time为UNIX时间戳
	sched_init();	# 进程调度		# /kernal/sched.c
	buffer_init(buffer_memory_end);		# /fs/buffer.c
	hd_init();		# 硬盘		# /
	floppy_init();	# 软盘,直接忽略
				
	sti();			# 调用sti汇编指令，表示允许中断，所以上面trap_init只是设置了idt，但直到此时，才可以使用中断	

					# 利用中断返回切换到进程0继续执行
	move_to_user_mode();	# 切换到用户模式

					# 通过_syscall0(int,fork)调用fork函数，_syscall0定义在unistd.h
					# fork之后，出现了进程0和进程1两个进程，都执行相同的代码
					# 对于Linux种的fork调用：如果当前是父进程，则会返回子进程pid，如果当前是子进程，则会返回0
					# 所以只有进程1才能执行init调用
	if (!fork()) {		/* we count on this going ok */
		init();		
					
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
					### 死循环，如果没有任何任务可以运行，就一直循环
	for(;;) pause();
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
						# 这里的fd指定为1，即标准输出打印
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;
					### setup是系统调用函数，用于设置初始化硬盘信息，仅可执行一次
					# 在其中填充了hd_info数组，包含每个磁盘的属性信息。填充了hd数组，包含每个磁盘的分区信息
					# 最后调用mount_root()方法加载了根文件系统
					# drive_info为存储在0x90080处的硬盘参数表信息
	setup((void *) &drive_info);

					### open是系统调用，用于打开指定路径的文件，位于/fs/open.c中
					# dup函数复制filp中索引为0的项，现在filp[0] - filp[2]均指向file_table中的同一项
					# 即标准输入，标准输出，标准错误输出都指向了同一个文件，即tty0
	(void) open("/dev/tty0",O_RDWR,0);	# stdin
	(void) dup(0);						# stdout	/fs/fcntl.c
	(void) dup(0);						# stderr
					### 此时，进程1就具备了与外设的交互能力，这种能力可以通过fork调用传递给子进程

					### 向标准输出(tty0)打印信息
					# 打印缓冲区块数和总字节数，每块 1024 字节
					# 以及主内存区空闲内存字节数。
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);


					### 子进程(进程2)执行内容
	if (!(pid=fork())) {		
		close(0);								# 关闭0号fd
		if (open("/etc/rc",O_RDONLY,0))			# 以只读方式打开rc文件，fd为0
												# rc中存放了一些shell的配置信息
			_exit(1);							

		execve("/bin/sh",argv_rc,envp_rc);		# 将进程自身替换成/bin/sh程序
							# 宏定义	/lib/execve.c			 
							# 系统调用 	/include/unistd.h		 _syscall3 
							# 汇编实现 	/kernal/system_call.s	 _sys_execve
							# C实现    /fs/exec.c 				do_execve

		_exit(2);
	}


					### 进程1继续执行
					
					# shell在标准输入为文件时，读取完文件中的命令就会退出，标准输入为字符设备时，则会等待
					# 上面的进程2将rc文件作为shell的标准输入，因此进程2在执行完文件中的命令后就会退出
					# 因此进程2可以被用来做一些初始化操作，例如用户登录

					# 这里需要等待进程2执行完毕
					# wait返回值是子进程pid，i中存放返回状态信息(返回码)
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;


					# 进程1进入死循环，不再退出，不断地
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
					# 子进程可执行
		if (!pid) {
			close(0);close(1);close(2);				# 关闭父进程传递来的fd
			setsid();								# 新的会话session
			(void) open("/dev/tty0",O_RDWR,0);		# 重新设置fd，和父进程的不相干扰
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));		# 执行shell程序
		}

					# 等待子进程执行完毕
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		
					# 同步操作，刷新缓冲区
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
