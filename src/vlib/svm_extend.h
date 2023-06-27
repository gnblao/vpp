
#ifndef __included_svm_extend_h_
#define __included_svm_extend_h_

#include <svm/fifo_types.h>
#include <vlib/vlib.h>

int svm_fifo_enqueue_w_buffer (svm_fifo_t * f, vlib_buffer_t *b);
int svm_fifo_enqueue_w_buffer_with_offset (svm_fifo_t * f, u32 offset, vlib_buffer_t *b);

int svm_fifo_peek_w_buffer (svm_fifo_t *f, u32 offset, u32 len, u8 *dst);
int svm_fifo_dequeue_w_buffer (svm_fifo_t * f, u32 len, u8 * dst);
int svm_fifo_dequeue_drop_w_buffer (svm_fifo_t * f, u32 len);


#endif
