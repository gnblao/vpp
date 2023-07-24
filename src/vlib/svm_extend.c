
#include <svm/svm_fifo.h>
#include <vlib/vlib.h>

static u8 *
format_b_seg(u8 *s, va_list *va) {
    svm_fifo_buffer_seg_t *b_seg;
    b_seg = va_arg (*va, void *); 
    
    s = format(s, "b_seg->bi:%u ", b_seg->bi);
    s = format(s, "b_seg->start:%u ", b_seg->start);
    s = format(s, "b_seg->length:%u ", b_seg->length);
    s = format(s, "b_seg->debug:%u ", b_seg->debug);

    return s;
}

static u8 *
format_vlib_buffer_2 (u8 * s, va_list * args)
{
  vlib_main_t *vm = vlib_get_first_main ();
  vlib_buffer_t *b = va_arg (*args, vlib_buffer_t *);
  u32 indent = format_get_indent (s);

  s = format (s, "%U", format_vlib_buffer_no_chain, b);

  while (b->flags & VLIB_BUFFER_NEXT_PRESENT)
    {
      u32 next_buffer = b->next_buffer;
      b = vlib_get_buffer (vm, next_buffer);

      s =
	format (s, "\n%Unext-buffer 0x%x, segment length %d, ref-count %u",
		format_white_space, indent, next_buffer, b->current_length,
		b->ref_count);
    }

  return s;
}

u8 *
format_vlib_buffer_and_data_2 (u8 * s, va_list * args)
{
  vlib_buffer_t *b = va_arg (*args, vlib_buffer_t *);

  s = format (s, "%U, %U",
	      format_vlib_buffer_2, b,
	      format_hex_bytes, vlib_buffer_get_current (b), 64);

  return s;
}

static inline u32
svm_fifo_max_dequeue_cons_w_buffer (svm_fifo_t * f)
{
  u32 tail, head, cursize;
  f_load_head2_tail2_cons (f, &head, &tail);

  /* current size of fifo can only increase during dequeue: SPSC */
  cursize = f_cursize (f, head, tail);
  
  if (f->cache_buffer && f->cache_length)
      cursize += f->cache_length - f->cache_pos;

  return cursize;
}

inline u32 
svm_fifo_max_dequeue_cons_maybe_buffer (svm_fifo_t * f) {
    if (f->flags & SVM_FIFO_F_LL_BUFFER)
        return svm_fifo_max_dequeue_cons_w_buffer(f);

    return svm_fifo_max_dequeue_cons(f);
}

inline int
svm_fifo_is_empty_cons_maybe_buffer (svm_fifo_t * f)
{
  return (svm_fifo_max_dequeue_cons_maybe_buffer (f) == 0);
}

int
svm_fifo_enqueue_w_buffer (svm_fifo_t * f, vlib_buffer_t *b)
{
  u32 tail, head, free_count, len;
  svm_fifo_buffer_seg_t b_seg_, *b_seg=&b_seg_;


  len = vlib_buffer_length_in_chain(vlib_get_main(), b);

  f_load_head2_tail2_prod (f, &head, &tail);

  /* free space in fifo can only increase during enqueue: SPSC */
  free_count = f_free_count (f, head, tail);

  if (PREDICT_FALSE (free_count < len))
    return SVM_FIFO_EFULL;

  b_seg->bi = vlib_get_buffer_index(vlib_get_main(), b);
  b_seg->start = tail;
  b_seg->length = len;
  
//  static u32 g_id = 0;
//  b_seg->debug = g_id++;
//  
//  clib_warning("-----enqueue-------%U----[%U]", format_b_seg, b_seg, format_vlib_buffer_and_data_2, b);

  svm_fifo_enqueue(f, sizeof( * b_seg), (u8* ) b_seg);
  
  tail = tail + len;

  svm_fifo_trace_add (f, head, len, 2);

  /* collect out-of-order segments */
  if (PREDICT_FALSE (f->ooos_list_head != OOO_SEGMENT_INVALID_INDEX))
    {
      len += ooo_segment_try_collect (f, len, &tail);
    }

  /* store-rel: producer owned index (paired with load-acq in consumer) */
  clib_atomic_store_rel_n (&f->shr->tail2, tail);

  return len;
}

int
svm_fifo_enqueue_w_buffer_with_offset (svm_fifo_t * f, u32 offset, vlib_buffer_t *b)
{
  u32 tail, head, free_count, len;
  ooo_segment_t *s;  

  f_load_head2_tail2_prod (f, &head, &tail);

  /* free space in fifo can only increase during enqueue: SPSC */
  free_count = f_free_count (f, head, tail);
  f->ooos_newest = OOO_SEGMENT_INVALID_INDEX;

  len = vlib_buffer_length_in_chain(vlib_get_main(), b);

  /* will this request fit? */
  if ((len + offset) > free_count)
    return SVM_FIFO_EFULL;

  svm_fifo_trace_add (f, offset, len, 1);
  ooo_segment_add (f, offset, head, tail, len);
  
  s = pool_elt_at_index (f->ooo_segments, f->ooos_newest);
  s->bi = vlib_get_buffer_index(vlib_get_main(), b);

  return 0;
}

int
svm_fifo_dequeue_w_buffer (svm_fifo_t * f, u32 len, u8 * dst)
{
  u32 tail, head, cursize;
  u32 to_copy = 0;
  u32 n, chain_in_off = 0;
  
  vlib_main_t *vm = vlib_get_first_main();
  vlib_buffer_t *chain_b;
  svm_fifo_buffer_seg_t b_seg_, *b_seg=&b_seg_;

  f_load_head2_tail2_cons (f, &head, &tail);

  /* current size of fifo can only increase during dequeue: SPSC */
  cursize = f_cursize (f, head, tail);
  
  if (f->cache_buffer && !f->cache_length)
      f->cache_length = vlib_buffer_length_in_chain(vm, f->cache_buffer);

  cursize += f->cache_length - f->cache_pos;

  if (PREDICT_FALSE (cursize == 0))
      return SVM_FIFO_EEMPTY;

  len = clib_min (cursize, len);

  chain_b = f->cache_buffer;
  while (to_copy < len)
    {
      if (!f->cache_buffer || f->cache_length <= f->cache_pos)
	{
	  if (svm_fifo_dequeue (f, sizeof (*b_seg), (u8 *) b_seg) != sizeof(*b_seg))
              break;

	  /* store-rel: consumer owned index (paired with load-acq in producer)
	   */
	  clib_atomic_store_rel_n (&f->shr->head2,
				   b_seg->start + b_seg->length);

	  if (f->cache_buffer) {
              vec_add1 (f->free_buffers, vlib_get_buffer_index (vm, f->cache_buffer));
          }

	  f->cache_buffer = vlib_get_buffer (vm, b_seg->bi);
	  f->cache_pos = 0;
	  f->cache_length =
	    vlib_buffer_length_in_chain (vm, f->cache_buffer);
     
          if (PREDICT_FALSE(f->cache_length != b_seg->length))
              clib_warning("-----bug-------%U----[%U]", format_b_seg, b_seg, format_vlib_buffer_and_data_2, f->cache_buffer);

          chain_b = f->cache_buffer;
          chain_in_off = 0;
	}
      
      if (chain_in_off + chain_b->current_length <= f->cache_pos)
	{
	  chain_in_off += chain_b->current_length;

	  if (chain_b->next_buffer)
	  {
	      chain_b = vlib_get_buffer (vm, chain_b->next_buffer);
	      continue;
	  }
	}

      n = len - to_copy;
      n = clib_min (n, chain_in_off + chain_b->current_length - f->cache_pos);

      clib_memcpy_fast (
	dst + to_copy,
	vlib_buffer_get_current (chain_b) + (f->cache_pos - chain_in_off), n);

      to_copy += n;
      f->cache_pos += n;
    }

  return len;
}

int
svm_fifo_peek_w_buffer (svm_fifo_t *f, u32 offset, u32 len, u8 *dst)
{
  u32 tail, head, cursize;
  u32 to_copy = 0, head_offset = 0, peek_offset = 0;
  u32 cache_pos = 0, cache_len = 0;
  vlib_buffer_t *cache_b;

  u32 n, chain_in_off = 0;
  vlib_buffer_t *chain_b;
  vlib_main_t *vm = vlib_get_first_main();

  svm_fifo_buffer_seg_t b_seg_, *b_seg = &b_seg_;

  f_load_head2_tail2_cons (f, &head, &tail);

  /* current size of fifo can only increase during dequeue: SPSC */
  cursize = f_cursize (f, head, tail);

  cache_b = f->cache_buffer;
  cache_pos = f->cache_pos;
  if (cache_b)
    cache_len =
      vlib_buffer_length_in_chain (vm, cache_b);

  cursize += cache_len - cache_pos;

  if (PREDICT_FALSE (cursize - offset <= 0))
    return SVM_FIFO_EEMPTY;

  len = clib_min (cursize - offset, len);

  peek_offset += cache_len - cache_pos;
  chain_b = cache_b;
  while (to_copy < len)
    {
      if (!cache_b || peek_offset <= offset)
	{
	  svm_fifo_peek (f, head_offset, sizeof (*b_seg), (u8 *) b_seg);
	  head_offset += sizeof (*b_seg);

	  cache_b = vlib_get_buffer (vm, b_seg->bi);
	  cache_len = vlib_buffer_length_in_chain (vm, cache_b);
	  cache_pos = 0;

	  if (peek_offset + cache_len <= offset)
	    {
	      peek_offset += cache_len;
	      continue;
	    }
      
          chain_b = cache_b;
          chain_in_off = 0;
	}

      if ((chain_in_off + chain_b->current_length) <= cache_pos)
	{
	  chain_in_off += chain_b->current_length;

	  chain_b = vlib_get_buffer (vm, chain_b->next_buffer);
	  continue;
	}

      n = len - to_copy;
      n = clib_min (n, chain_in_off + chain_b->current_length - cache_pos);

      clib_memcpy_fast (
	dst + to_copy,
	vlib_buffer_get_current (chain_b) + (cache_pos - chain_in_off), n);

      to_copy += n;
      cache_pos += n;
    }

  return len;
}


int
svm_fifo_dequeue_drop_w_buffer (svm_fifo_t * f, u32 len)
{
  u32 total_drop_bytes, tail, head, cursize;
  u32 to_drop = 0, n = 0;
  svm_fifo_buffer_seg_t b_seg_, *b_seg = &b_seg_;

  vlib_main_t *vm = vlib_get_first_main();
  
  f_load_head2_tail2_cons (f, &head, &tail);

  /* current size of fifo can only increase during dequeue: SPSC */
  cursize = f_cursize (f, head, tail);

  cursize += (f->cache_length - f->cache_pos);

  if (PREDICT_FALSE (cursize == 0))
    return SVM_FIFO_EEMPTY;

  /* number of bytes we're going to drop */
  total_drop_bytes = clib_min (cursize, len);

  svm_fifo_trace_add (f, tail, total_drop_bytes, 3);

  while (to_drop < total_drop_bytes) {
    if (!f->cache_buffer || f->cache_pos >= f->cache_length) {
      svm_fifo_dequeue (f, sizeof (*b_seg), (u8 *) b_seg);

      /* store-rel: consumer owned index (paired with load-acq in producer) */
      clib_atomic_store_rel_n (&f->shr->head2, b_seg->start + b_seg->length);

      if (f->cache_buffer)
          vec_add1(f->free_buffers, vlib_get_buffer_index(vm, f->cache_buffer));
      
      f->cache_buffer = vlib_get_buffer(vm, b_seg->bi);
      f->cache_pos = 0;
      f->cache_length = b_seg->length;
    }

    n = total_drop_bytes - to_drop;
    n = clib_min(n, f->cache_length - f->cache_pos);
    to_drop += n;
    f->cache_pos += n;
  }
  
  return total_drop_bytes;
}


