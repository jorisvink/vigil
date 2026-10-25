/*
 * Copyright (c) 2023-2026 Joris Vink <joris@sanctorum.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include "vigil.h"

/*
 * A multi-producer, multi-consumer ring queue, taken from sanctum.
 */

/*
 * Initialise the given ring queue with the number of elements.
 * The number of elements must be a power of 2 and must maximum
 * be VIGIL_RING_MAX.
 */
void
vigil_ring_init(struct vigil_ring *ring, size_t elm)
{
	PRECOND(ring != NULL);
	PRECOND(elm > 0 && (elm & (elm - 1)) == 0);
	PRECOND(elm <= VIGIL_RING_MAX);

	memset(ring, 0, sizeof(*ring));

	ring->elm = elm;
	ring->mask = elm - 1;
}

/*
 * Returns the number of entries that are ready to be dequeued from the queue.
 * This is intended for the consumers of the ring queue.
 */
size_t
vigil_ring_pending(struct vigil_ring *ring)
{
	u_int32_t	head, tail;

	PRECOND(ring != NULL);

	head = vigil_atomic_read(&ring->consumer.head);
	tail = vigil_atomic_read(&ring->producer.tail);

	return (tail - head);
}

/*
 * Dequeue an item from the given ring queue. If no items were
 * available to be dequeued, -1 is returned to the caller.
 */
int
vigil_ring_dequeue(struct vigil_ring *ring, u_int32_t *idx)
{
	u_int32_t	slot, head, tail, next;

	PRECOND(ring != NULL);
	PRECOND(idx != NULL);

dequeue_again:
	head = vigil_atomic_read(&ring->consumer.head);
	tail = vigil_atomic_read(&ring->producer.tail);

	if ((tail - head) == 0)
		return (-1);

	next = head + 1;
	if (!vigil_atomic_cas(&ring->consumer.head, &head, &next))
		goto dequeue_again;

	slot = head & ring->mask & (VIGIL_RING_MAX - 1);
	*idx = vigil_atomic_read(&ring->data[slot]);

	while (!vigil_atomic_cas_simple(&ring->consumer.tail, head, next))
		vigil_cpu_pause();

	return (0);
}

/*
 * Queue the given item into the given ring queue. If no available
 * slots were available, this function will return -1.
 */
int
vigil_ring_queue(struct vigil_ring *ring, u_int32_t idx)
{
	u_int32_t	slot, head, tail, next;

	PRECOND(ring != NULL);

queue_again:
	head = vigil_atomic_read(&ring->producer.head);
	tail = vigil_atomic_read(&ring->consumer.tail);

	if ((ring->elm + (tail - head)) == 0)
		return (-1);

	next = head + 1;
	if (!vigil_atomic_cas(&ring->producer.head, &head, &next))
		goto queue_again;

	slot = head & ring->mask & (VIGIL_RING_MAX - 1);
	vigil_atomic_write(&ring->data[slot], idx);

	while (!vigil_atomic_cas_simple(&ring->producer.tail, head, next))
		vigil_cpu_pause();

	return (0);
}
