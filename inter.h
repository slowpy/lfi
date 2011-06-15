/*
     Created by Paul Marinescu and George Candea
     Copyright (C) 2009 EPFL (Ecole Polytechnique Federale de Lausanne)

     This file is part of LFI (Library-level Fault Injector).

     LFI is free software: you can redistribute it and/or modify it  
     under the terms of the GNU General Public License as published by the  
     Free Software Foundation, either version 3 of the License, or (at  
     your option) any later version.

     LFI is distributed in the hope that it will be useful, but  
     WITHOUT ANY WARRANTY; without even the implied warranty of  
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU  
     General Public License for more details.

     You should have received a copy of the GNU General Public  
     License along with LFI. If not, see http://www.gnu.org/licenses/.

     EPFL
     Dependable Systems Lab (DSLAB)
     Room 330, Station 14
     1015 Lausanne
     Switzerland
*/

#include <errno.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef __USE_GNU
#define __USE_GNU
#endif

#include <dlfcn.h>
#include <execinfo.h>
#include <time.h>

class Trigger;

/* the maximum number of frames in a stack trace */
#define TRACE_SIZE	100
/* human readable log file (overwritten at each run) */
#define LOGFILE		"inject.log"
#define LOGGING		0
/* machine readable log file used for injection replay (overwritten at each run) */
#define	REPLAYFILE	"replay.xml"

#define MAXINJECT	2000000

/* only used to enhance readbility */
#ifndef __in
#define __in
#endif

#ifndef __out
#define __out
#endif

#ifndef NULL
#define NULL	(void*)0
#endif

struct TriggerDesc
{
	char id[128];
	char tclass[128];
	Trigger* trigger;
	char init[4096];
};

struct fninfov2
{
	char function_name[256];
	int return_value;
	int errno_value;
	int call_original;
	int argc;

	/* custom triggers */
	TriggerDesc **triggers;
};

/* stores the return address across the original library function call */
static __thread long return_address;
/* avoid intercepting our function calls */
static __thread int no_intercept;

/*
   avoid including the standard headers because the compiler will likely
   generate warnings when injecting faults in an already declared function.
   However, this won't allow us to inject faults in these functions
   ALTERNATIVE: use dlsym to get pointers to the functions
*/

extern "C" {
	int printf(const char * _Format, ...);
}

void determine_action(struct fninfov2 fn_details[],
					  __in const char* function_name,
					  __out int* call_original,
					  __out int *return_error,
					  __out int* return_code,
					  __out int* return_errno);

void print_backtrace(void* bt[], int nptrs, int log_fd);

/************************************************************************/
/*	GENERATE_STUBv2 - macro to generate stub functions targeted for x86 */
/*	UNSAFE: clobbered non-volatile registeres may not be restored       */
/*	  when `forcing' an exit (i.e. when doing a leave/ret or leave/jmp) */
/*	SOLUTION: manually push/pop all non-volatile registers OR           */
/*	  check assembly listing generated by the compiler to determine if  */
/*	  any registers need to be `pop`-ed (per-compiler solution)         */
/************************************************************************/
#define GENERATE_STUBv2(FUNCTION_NAME) \
	void FUNCTION_NAME (void) \
{ \
	int call_original, return_error; \
	int return_code, return_errno; \
	int initial_no_intercept; \
	static void * (*original_fn_ptr)(); \
	\
	/* defaults */ \
	call_original = 1; \
	return_error = 0; \
	return_code = 0; \
	return_errno = 0; \
	\
	initial_no_intercept = no_intercept; \
	if (0 == no_intercept && init_done /* don't hook open or write in the constructor */) { \
		no_intercept = 1; /* allow all determine_action-called functions to pass-through */ \
		determine_action(function_info_ ## FUNCTION_NAME, #FUNCTION_NAME, &call_original, &return_error, &return_code, &return_errno); \
	} \
	\
	if(!original_fn_ptr) { \
		original_fn_ptr = (void *(*)()) dlsym(RTLD_NEXT, #FUNCTION_NAME); \
		if(!original_fn_ptr) \
			printf("Unable to get address for function %s\n", #FUNCTION_NAME); \
	} \
	\
	no_intercept = initial_no_intercept; \
	/* disabled - unlikely to be useful in practice */ \
	if (0 && call_original && return_error) \
	{ \
		/* save the original return value */ \
		__asm__ ("movl 0x4(%%ebp), %%eax;" : "=a"(return_address)); \
		\
		__asm__ ("leave"); \
		__asm__ ("addl $0x4, %esp"); \
		/* at this point the stack is gone */ \
		\
		/* make the call the original function with the same stack */ \
		__asm__ ("call *%%eax;" : : "a"(original_fn_ptr)); \
		\
		errno = return_errno; \
		\
		/* push back the original return value */ \
		__asm__ ("pushl %%eax;" : : "a"(return_address)); \
		\
		__asm__ ("ret" : : "a"(return_code)); \
	} \
	else if (return_error) \
	{ \
		errno = return_errno; \
		__asm__ ("nop" : : "a"(return_code)); \
		return; \
	} \
	else if (call_original) \
	{ \
		/* this must correspond to the compiler-generated prologue for this function */ \
		__asm__ ("nop" : : "a"(original_fn_ptr)); \
		__asm__ ("mov %ebp, %esp"); \
		__asm__ ("sub $0x4, %esp"); /* assuming the compiler-generated prologue saves only ebx */ \
		__asm__ ("pop %ebx"); \
		__asm__ ("pop %ebp"); \
		__asm__ ("jmp *%eax"); \
	} \
}

/************************************************************************/
/*	GENERATE_STUBv2_x64 - macro to generate stub functions on x64       */
/*	UNSAFE: can present undefined behaviour when an exception is        */
/*         encountered here or in any called function                   */
/*	(but it's ok if an exception happens in the original function)      */
/*	PROBLEM: the x64 ABI has a clear notion of function prologue and    */
/*                                                   epilogue but...    */
/*		we are using the stack to save/restore the initial state        */
/*      (the one where we were called), i.e. register values,           */
/*      outside the prologue/epilogue                                   */
/*		http://msdn.microsoft.com/en-us/library/8ydc79k6(VS.80).aspx    */
/*      TODO: write the entire function in assembly                     */
/************************************************************************/

#define GENERATE_STUBv2_x64(FUNCTION_NAME) \
	void FUNCTION_NAME (void) \
{ \
	int nptrs; \
	void* buffer[TRACE_SIZE]; \
	int call_original, return_error; \
	int return_code, return_errno; \
	static void * (*original_fn_ptr)(); \
	/* we can't call write directly because it would prevent us for injecting faults in `write`
       (injecting requires the creation of a function with the same name but the prototype is
	   different). We, use dlsym instead
	*/ \
	static int * (*original_write_ptr)(int, const void*, int); \
	int initial_no_intercept; \
	\
	/* save non-volatiles */ \
	__asm__ ("push %r15"); \
	__asm__ ("push %r14"); \
	__asm__ ("push %r13"); \
	__asm__ ("push %r12"); \
	__asm__ ("push %rdi"); \
	__asm__ ("push %rsi"); \
	__asm__ ("push %rbx"); \
	\
	/* save function arguments */ \
	__asm__ ("push %rcx"); \
	__asm__ ("push %rdx"); \
	__asm__ ("push %r8"); \
	__asm__ ("push %r9"); \
	\
	/* defaults */ \
	call_original = 1; \
	return_error = 0; \
	return_code = 0; \
	return_errno = 0; \
	nptrs = 0; \
	/* printf("intercepted %s\n", #FUNCTION_NAME); */\
	\
	initial_no_intercept = no_intercept; \
	if (0 == no_intercept) { \
		no_intercept = 1; \
		determine_action(function_info_ ## FUNCTION_NAME, #FUNCTION_NAME, &call_original, &return_error, &return_code, &return_errno); \
	} \
        \
	\
	if(!original_fn_ptr) { \
		original_fn_ptr = (void *(*)()) dlsym(RTLD_NEXT, #FUNCTION_NAME); \
		if(!original_fn_ptr) \
			printf("Unable to get address for function %s\n", #FUNCTION_NAME); \
	} \
	\
	no_intercept = initial_no_intercept; \
	\
	if (return_error) \
	{ \
		errno = return_errno; \
		__asm__ ("nop" : : "a"(return_code)); \
		/*__asm__ ("pop %rbx"); \
		__asm__ ("pop %rsi"); \
		__asm__ ("pop %rdi"); \
		__asm__ ("pop %r12"); \
		__asm__ ("pop %r13"); \
		__asm__ ("pop %r14"); \
		__asm__ ("pop %r15"); */ \
		/* let the compiler-generated epilogue restore everything */ \
		/* just clean up our pushes: 11pushes x 8bytes = 0x58 bytes */ \
		__asm__ ("add $0x58, %rsp"); \
		return; \
	} \
	else if (call_original) \
	{ \
		/* restore function arguments */ \
		__asm__ ("pop %r9"); \
		__asm__ ("pop %r8"); \
		__asm__ ("pop %rdx"); \
		__asm__ ("pop %rcx"); \
		/* restore non-volatiles */ \
		__asm__ ("pop %rbx"); \
		__asm__ ("pop %rsi"); \
		__asm__ ("pop %rdi"); \
		__asm__ ("pop %r12"); \
		__asm__ ("pop %r13"); \
		__asm__ ("pop %r14"); \
		__asm__ ("pop %r15"); \
		__asm__ ("leave"); \
		__asm__ ("jmp *%%rax" : : "a"(original_fn_ptr)); \
	} \
}


#define STUB_VAR_DECL \
int log_fd, replay_fd; \
int init_done; 
