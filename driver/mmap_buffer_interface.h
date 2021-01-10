#ifndef _MMAP_BUFFER_INTERFACE_
#define _MMAP_BUFFER_INTERFACE_

#include "mmap_generational_fifo_buffer.h"
#include "mmap_banked_buffer.h"

/* Define a standard interface into the buffering scheme using macros that are later redefined */

/* Found this nice macro trick for type-based overloading in C: http://locklessinc.com/articles/overloading/ */

// warning: dereferencing type-punned pointer will break strict-aliasing rules
/* #define gcc_overload(A)\ */
/* 	__builtin_choose_expr(__builtin_types_compatible_p(typeof(A), struct s1),\ */
/* 		gcc_overload_s1(*(struct s1 *)&A),\ */
/* 	__builtin_choose_expr(__builtin_types_compatible_p(typeof(A), struct s2),\ */
/* 		gcc_overload_s2(*(struct s2 *)&A),(void)0)) */

/* Setting up and tearing down buffers */
#define init_mmap_buffer_data(T, A, B)                                 \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
                          init_mmap_buffer_data_fifo_buffer_t(*(fifo_buffer_t **)&T, A, B), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
                          init_mmap_buffer_data_banked_buffer_t(*(banked_buffer_t **)&T, A, B),(void)0))
						  
#define cleanup_mmap_buffer_data(T) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      cleanup_mmap_buffer_data_fifo_buffer_t(*(fifo_buffer_t **)&T),	\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      cleanup_mmap_buffer_data_banked_buffer_t(*(banked_buffer_t **)&T),(void)0))

/* Managing parameters */
#define reset_params(T) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      reset_params_fifo_buffer_t(*(fifo_buffer_t **)&T),	\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      reset_params_banked_buffer_t(*(banked_buffer_t **)&T),(void)0))

#define init_params(T, A) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      init_params_fifo_buffer_t(*(fifo_buffer_t **)&T, A),	\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      init_params_banked_buffer_t(*(banked_buffer_t **)&T, A),(void)0))

#define cleanup_params(T) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      cleanup_params_fifo_buffer_t(*(fifo_buffer_t **)&T), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      cleanup_params_banked_buffer_t(*(banked_buffer_t **)&T),(void)0))

/* Handle adding, deleting, etc pages from the buffer */
#define change_page_priority(T, A, B, C) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      change_page_priority_fifo_buffer_t(*(fifo_buffer_t **)&T, A, B, C), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      change_page_priority_banked_buffer_t(*(banked_buffer_t **)&T, A, B, C),(void)0))

#define hit_page(T, A)	\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
                        hit_page_fifo_buffer_t(*(fifo_buffer_t **)&T, A), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
                        hit_page_banked_buffer_t(*(banked_buffer_t **)&T, A),(void)0))

/* Query state of page in buffer */
#define page_in_buffer(T, A) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      page_in_buffer_fifo_buffer_t(*(fifo_buffer_t **)&T, A), \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      page_in_buffer_banked_buffer_t(*(banked_buffer_t **)&T, A),(void)0))

/* Reset or tune the buffer state */
#define reset_state(T) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
		reset_state_fifo_buffer_t(*(fifo_buffer_t **)&T),\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
		reset_state_banked_buffer_t(*(banked_buffer_t **)&T),(void)0))

#define reset_history(T) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
		reset_history_fifo_buffer_t(*(fifo_buffer_t **)&T),\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
		reset_history_banked_buffer_t(*(banked_buffer_t **)&T),(void)0))

#define print_status(T)	\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
		print_status_fifo_buffer_t(*(fifo_buffer_t **)&T),\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
		print_status_banked_buffer_t(*(banked_buffer_t **)&T),(void)0))

#define update_runtime_parameters(T, A) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      update_runtime_parameters_fifo_buffer_t(*(fifo_buffer_t **)&T, A),	\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      update_runtime_parameters_banked_buffer_t(*(banked_buffer_t **)&T, A),(void)0))

#define init_specific_counters(T) \
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), fifo_buffer_t *),\
			      init_specific_counters_fifo_buffer_t(*(fifo_buffer_t **)&T),	\
	__builtin_choose_expr(__builtin_types_compatible_p(typeof(T), banked_buffer_t *),\
			      init_specific_counters_banked_buffer_t(*(banked_buffer_t **)&T),(void)0))

#endif /* _MMAP_BUFFER_INTERFACE_ */
