/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2018 MonetDB B.V.
 */

/*
 * - Hash Table Creation
 * The hash indexing scheme for BATs reserves a block of memory to
 * maintain the hash table and a collision list. A one-to-one mapping
 * exists between the BAT and the collision list using the BUN
 * index. NOTE: we alloc the link list as a parallel array to the BUN
 * array; hence the hash link array has the same size as
 * BATcapacity(b) (not BATcount(b)). This allows us in the BUN insert
 * and delete to assume that there is hash space iff there is BUN
 * space. If there is no BUN space, the BATextend now destroys the
 * hash table.
 *
 * The hash mask size is a power of two, so we can do bitwise AND on
 * the hash (integer) number to quickly find the head of the bucket
 * chain.  Clearly, the hash mask size is a crucial parameter. If we
 * know that the column is unique (tkey), we use direct hashing (mask
 * size ~= BATcount). Otherwise we dynamically determine the mask size
 * by starting out with mask size = BATcount/64 (just 1.5% of memory
 * storage overhead). Then we start building the hash table on the
 * first 25% of the BAT. As we aim for max-collisions list length of
 * 4, the list on 25% should not exceed length 1. So, if a small
 * number of collisssions occurs (mask/2) then we abandon the attempt
 * and restart with a mask that is 4 times larger. This converges
 * after three cycles to direct hashing.
 */

#include "monetdb_config.h"
#include "gdk.h"
#include "gdk_cand.h"
#include "gdk_private.h"

static int
HASHwidth(BUN hashsize)
{
	if (hashsize <= (BUN) BUN2_NONE)
		return BUN2;
#if SIZEOF_BUN <= 4
	return BUN4;
#else
	if (hashsize <= (BUN) BUN4_NONE)
		return BUN4;
	return BUN8;
#endif
}

BUN
HASHmask(BUN cnt)
{
	BUN m = cnt;

	/* find largest power of 2 smaller than or equal to cnt */
	m |= m >> 1;
	m |= m >> 2;
	m |= m >> 4;
	m |= m >> 8;
	m |= m >> 16;
#if SIZEOF_BUN == 8
	m |= m >> 32;
#endif
	m -= m >> 1;

	/* if cnt is more than 1/3 into the gap between m and 2*m,
	   double m */
	if (m + m - cnt < 2 * (cnt - m))
		m += m;
	if (m < BATTINY)
		m = BATTINY;
	return m;
}

static void
HASHclear(Hash *h)
{
	/* since BUN2_NONE, BUN4_NONE, BUN8_NONE
	 * are all equal to -1 (~0), i.e., have all bits set,
	 * we can use a simple memset() to clear the Hash,
	 * rather than iteratively assigning individual
	 * BUNi_NONE values in a for-loop
	 */
	memset(h->Hash, 0xFF, (h->mask + 1) * h->width);
}

#define HASH_VERSION		2
#define HASH_HEADER_SIZE	6 /* nr of size_t fields in header */

gdk_return
HASHnew(Hash *h, int tpe, BUN size, BUN mask, BUN count)
{
	Heap *hp = &h->heap;
	int width = HASHwidth(size);

	if (HEAPalloc(hp, mask + size + HASH_HEADER_SIZE * SIZEOF_SIZE_T / width, width) != GDK_SUCCEED)
		return GDK_FAIL;
	h->heap.free = (mask + size) * width + HASH_HEADER_SIZE * SIZEOF_SIZE_T;
	h->lim = size;
	h->mask = mask - 1;
	h->width = width;
	switch (width) {
	case BUN2:
		h->nil = (BUN) BUN2_NONE;
		break;
	case BUN4:
		h->nil = (BUN) BUN4_NONE;
		break;
#ifdef BUN8
	case BUN8:
		h->nil = (BUN) BUN8_NONE;
		break;
#endif
	default:
		assert(0);
	}
	h->Link = h->heap.base + HASH_HEADER_SIZE * SIZEOF_SIZE_T;
	h->Hash = (void *) ((char *) h->Link + h->lim * width);
	h->type = tpe;
	HASHclear(h);		/* zero the mask */
	((size_t *) h->heap.base)[0] = HASH_VERSION;
	((size_t *) h->heap.base)[1] = size;
	((size_t *) h->heap.base)[2] = mask;
	((size_t *) h->heap.base)[3] = width;
	((size_t *) h->heap.base)[4] = count;
	((size_t *) h->heap.base)[5] = 0; /* # filled slots (chain heads) */
	ALGODEBUG fprintf(stderr, "#HASHnew: create hash(size " BUNFMT ", mask " BUNFMT ", width %d, nil " BUNFMT ", total " BUNFMT " bytes);\n", size, mask, width, h->nil, (size + mask) * width);
	return GDK_SUCCEED;
}

/* collect HASH statistics for analysis */
static void
HASHcollisions(BAT *b, Hash *h)
{
	lng cnt, entries = 0, max = 0;
	double total = 0;
	BUN p, i, j, nil;

	if (b == 0 || h == 0)
		return;
	nil = HASHnil(h);
	for (i = 0, j = h->mask; i <= j; i++)
		if ((p = HASHget(h, i)) != nil) {
			entries++;
			cnt = 0;
			for (; p != nil; p = HASHgetlink(h, p))
				cnt++;
			if (cnt > max)
				max = cnt;
			total += cnt;
		}
	fprintf(stderr, "#BAThash: statistics (" BUNFMT ", entries " LLFMT ", mask " BUNFMT ", max " LLFMT ", avg %2.6f);\n", BATcount(b), entries, h->mask, max, entries == 0 ? 0 : total / entries);
}

/* Return TRUE if we have a hash on the tail, even if we need to read
 * one from disk.
 *
 * Note that the b->thash pointer can be NULL, meaning there is no
 * hash; (Hash *) 1, meaning there is no hash loaded, but it may exist
 * on disk; or a valid pointer to a loaded hash.  These values are
 * maintained here, in the HASHdestroy and HASHfree functions, and in
 * BBPdiskscan during initialization. */
bool
BATcheckhash(BAT *b)
{
	bool ret;
	lng t = 0;

	ALGODEBUG t = GDKusec();
	MT_lock_set(&GDKhashLock(b->batCacheid));
	ALGODEBUG t = GDKusec() - t;
	if (b->thash == (Hash *) 1) {
		Hash *h;
		const char *nme = BBP_physical(b->batCacheid);
		int fd;

		b->thash = NULL;
		if ((h = GDKzalloc(sizeof(*h))) != NULL &&
		    (h->heap.farmid = BBPselectfarm(b->batRole, b->ttype, hashheap)) >= 0) {
			snprintf(h->heap.filename, sizeof(h->heap.filename),
				 "%s.thash", nme);

			/* check whether a persisted hash can be found */
			if ((fd = GDKfdlocate(h->heap.farmid, nme, "rb+", "thash")) >= 0) {
				size_t hdata[HASH_HEADER_SIZE];
				struct stat st;

				if (read(fd, hdata, sizeof(hdata)) == sizeof(hdata) &&
				    hdata[0] == (
#ifdef PERSISTENTHASH
					    ((size_t) 1 << 24) |
#endif
					    HASH_VERSION) &&
				    hdata[4] == (size_t) BATcount(b) &&
				    fstat(fd, &st) == 0 &&
				    st.st_size >= (off_t) (h->heap.size = h->heap.free = (hdata[1] + hdata[2]) * hdata[3] + HASH_HEADER_SIZE * SIZEOF_SIZE_T) &&
				    HEAPload(&h->heap, nme, "thash", 0) == GDK_SUCCEED) {
					h->lim = (BUN) hdata[1];
					h->type = ATOMtype(b->ttype);
					h->mask = (BUN) (hdata[2] - 1);
					h->width = (int) hdata[3];
					switch (h->width) {
					case BUN2:
						h->nil = (BUN) BUN2_NONE;
						break;
					case BUN4:
						h->nil = (BUN) BUN4_NONE;
						break;
#ifdef BUN8
					case BUN8:
						h->nil = (BUN) BUN8_NONE;
						break;
#endif
					default:
						assert(0);
					}
					h->Link = h->heap.base + HASH_HEADER_SIZE * SIZEOF_SIZE_T;
					h->Hash = (void *) ((char *) h->Link + h->lim * h->width);
					close(fd);
					h->heap.parentid = b->batCacheid;
					h->heap.dirty = FALSE;
					b->thash = h;
					ALGODEBUG fprintf(stderr, "#BATcheckhash: reusing persisted hash %s\n", BATgetId(b));
					MT_lock_unset(&GDKhashLock(b->batCacheid));
					return true;
				}
				close(fd);
				/* unlink unusable file */
				GDKunlink(h->heap.farmid, BATDIR, nme, "thash");
			}
		}
		GDKfree(h);
		GDKclrerr();	/* we're not currently interested in errors */
	}
	ret = b->thash != NULL;
	MT_lock_unset(&GDKhashLock(b->batCacheid));
	ALGODEBUG if (ret) fprintf(stderr, "#BATcheckhash: already has hash %s, waited " LLFMT " usec\n", BATgetId(b), t);
	return ret;
}

#ifdef PERSISTENTHASH
static void
BAThashsync(void *arg)
{
	BAT *b = arg;
	int fd;
	lng t0 = 0;
	const char *failed = " failed";

	ALGODEBUG t0 = GDKusec();

	MT_lock_set(&GDKhashLock(b->batCacheid));
	if (b->thash != NULL) {
		Heap *hp = &b->thash->heap;
		if (HEAPsave(hp, hp->filename, NULL) == GDK_SUCCEED &&
		    (fd = GDKfdlocate(hp->farmid, hp->filename, "rb+", NULL)) >= 0) {
			((size_t *) hp->base)[0] |= 1 << 24;
			if (write(fd, hp->base, SIZEOF_SIZE_T) >= 0) {
				failed = ""; /* not failed */
				if (!(GDKdebug & NOSYNCMASK)) {
#if defined(NATIVE_WIN32)
					_commit(fd);
#elif defined(HAVE_FDATASYNC)
					fdatasync(fd);
#elif defined(HAVE_FSYNC)
					fsync(fd);
#endif
				}
			} else {
				perror("write hash");
			}
			close(fd);
		}
		ALGODEBUG fprintf(stderr, "#BAThash: persisting hash %s (" LLFMT " usec)%s\n", hp->filename, GDKusec() - t0, failed);
	}
	MT_lock_unset(&GDKhashLock(b->batCacheid));
	BBPunfix(b->batCacheid);
}
#endif

#define starthash(TYPE)							\
	do {								\
		const TYPE *restrict v = (const TYPE *) BUNtloc(bi, 0);	\
		if (cand) {						\
			for (; p < cnt1; p++) {				\
				c = hash_##TYPE(h, v + cand[p] - b->hseqbase); \
				hget = HASHget(h, c);			\
				if (hget == hnil) {			\
					if (nslots == maxslots)		\
						break; /* mask too full */ \
					nslots++;			\
				}					\
				HASHputlink(h, p, hget);		\
				HASHput(h, c, p);			\
			}						\
		} else {						\
			v += start;					\
			for (; p < cnt1; p++) {				\
				c = hash_##TYPE(h, v + p);		\
				hget = HASHget(h, c);			\
				if (hget == hnil) {			\
					if (nslots == maxslots)		\
						break; /* mask too full */ \
					nslots++;			\
				}					\
				HASHputlink(h, p, hget);		\
				HASHput(h, c, p);			\
			}						\
		}							\
	} while (0)
#define finishhash(TYPE)						\
	do {								\
		const TYPE *restrict v = (const TYPE *) BUNtloc(bi, 0);	\
		if (cand) {						\
			for (; p < cnt; p++) {				\
				c = hash_##TYPE(h, v + cand[p] - b->hseqbase); \
				hget = HASHget(h, c);			\
				nslots += hget == hnil;			\
				HASHputlink(h, p, hget);		\
				HASHput(h, c, p);			\
			}						\
		} else {						\
			v += start;					\
			for (; p < cnt; p++) {				\
				c = hash_##TYPE(h, v + p);		\
				hget = HASHget(h, c);			\
				nslots += hget == hnil;			\
				HASHputlink(h, p, hget);		\
				HASHput(h, c, p);			\
			}						\
		}							\
	} while (0)

/*
 * The prime routine for the BAT layer is to create a new hash index.
 * Its argument is the element type and the maximum number of BUNs be
 * stored under the hash function.
 */
Hash *
BAThash_impl(BAT *b, BAT *s, BUN masksize, const char *ext)
{
	lng t0 = 0;
	unsigned int tpe = ATOMbasetype(b->ttype);
	BUN cnt, start, end, cnt1;
	const oid *cand, *candend;
	BUN mask, maxmask = 0;
	BUN p, c;
	BUN nslots;
	BUN hnil, hget;
	Hash *h = NULL;
	const char *nme = BBP_physical(b->batCacheid);
	BATiter bi = bat_iterator(b);

	ALGODEBUG t0 = GDKusec();
	ALGODEBUG fprintf(stderr, "#BAThash: create hash(" ALGOBATFMT ");\n",
			  ALGOBATPAR(b));
	if (b->ttype == TYPE_void) {
		if (is_oid_nil(b->tseqbase)) {
			ALGODEBUG fprintf(stderr, "#BAThash: cannot create hash-table on void-NIL column.\n");
			GDKerror("BAThash: no hash on void/nil column\n");
			return NULL;
		}
		ALGODEBUG fprintf(stderr, "#BAThash: creating hash-table on void column..\n");

		tpe = TYPE_void;
	}

	CANDINIT(b, s, start, end, cnt, cand, candend);
	cnt = cand ? (BUN) (candend - cand) : end - start;

	if ((h = GDKzalloc(sizeof(*h))) == NULL ||
	    (h->heap.farmid = BBPselectfarm(b->batRole, b->ttype, hashheap)) < 0) {
		GDKfree(h);
		return NULL;
	}
	h->heap.dirty = TRUE;
	snprintf(h->heap.filename, sizeof(h->heap.filename), "%s.%s", nme, ext);

	/* determine hash mask size */
	cnt1 = 0;
	if (masksize > 0) {
		/* specified by caller */
		mask = HASHmask(masksize);
	} else if (ATOMsize(tpe) == 1) {
		/* perfect hash for one-byte sized atoms */
		mask = (1 << 8);
	} else if (ATOMsize(tpe) == 2) {
		/* perfect hash for two-byte sized atoms */
		mask = (1 << 16);
	} else if (b->tkey || cnt <= 4096) {
		/* if key, or if small, don't bother dynamically
		 * adjusting the hash mask */
		mask = HASHmask(cnt);
 	} else {
		/* dynamic hash: we start with HASHmask(cnt)/64, or,
		 * if cnt large enough, HASHmask(cnt)/256; if there
		 * are too many collisions we try HASHmask(cnt)/64,
		 * HASHmask(cnt)/16, HASHmask(cnt)/4, and finally
		 * HASHmask(cnt), but we might skip some of these if
		 * there are many distinct values.  */
		maxmask = HASHmask(cnt);
		mask = maxmask >> 6;
		while (mask > 4096)
			mask >>= 2;
		/* try out on first 25% of b */
		cnt1 = cnt >> 2;
	}

	for (;;) {
		BUN maxslots = (mask >> 3) - 1;	/* 1/8 full is too full */

		nslots = 0;
		p = 0;
		HEAPfree(&h->heap, 1);
		/* create the hash structures */
		if (HASHnew(h, ATOMtype(b->ttype), s ? cnt : BATcapacity(b),
			    mask, cnt) != GDK_SUCCEED) {

			GDKfree(h);
			return NULL;
		}

		hnil = HASHnil(h);

		switch (tpe) {
		case TYPE_bte:
			starthash(bte);
			break;
		case TYPE_sht:
			starthash(sht);
			break;
		case TYPE_flt:
			starthash(flt);
			break;
		case TYPE_int:
			starthash(int);
			break;
		case TYPE_dbl:
			starthash(dbl);
			break;
		case TYPE_lng:
			starthash(lng);
			break;
#ifdef HAVE_HGE
		case TYPE_hge:
			starthash(hge);
			break;
#endif
		default:
			if (cand) {
				for (; p < cnt1; p++) {
					const void *restrict v = BUNtail(bi, cand[p] - b->hseqbase);
					c = heap_hash_any(b->tvheap, h, v);
					hget = HASHget(h, c);
					if (hget == hnil) {
						if (nslots == maxslots)
							break; /* mask too full */
						nslots++;
					}
					HASHputlink(h, p, hget);
					HASHput(h, c, p);
				}
			} else {
				for (; p < cnt1; p++) {
					const void *restrict v = BUNtail(bi, p + start);
					c = heap_hash_any(b->tvheap, h, v);
					hget = HASHget(h, c);
					if (hget == hnil) {
						if (nslots == maxslots)
							break; /* mask too full */
						nslots++;
					}
					HASHputlink(h, p, hget);
					HASHput(h, c, p);
				}
			}
			break;
		}
		ALGODEBUG if (p < cnt1)
			fprintf(stderr, "#BAThash(%s): abort starthash with "
				"mask " BUNFMT " at " BUNFMT "\n", BATgetId(b),
				mask, p);
		if (p == cnt1 || mask == maxmask)
			break;
		mask <<= 2;
		/* if we fill up the slots fast (p <= maxslots * 1.2)
		 * increase mask size a bit more quickly */
		if (mask < maxmask && p <= maxslots * 1.2)
			mask <<= 2;
	}

	/* finish the hashtable with the current mask */
	switch (tpe) {
	case TYPE_bte:
		finishhash(bte);
		break;
	case TYPE_sht:
		finishhash(sht);
		break;
	case TYPE_int:
		finishhash(int);
		break;
	case TYPE_flt:
		finishhash(flt);
		break;
	case TYPE_dbl:
		finishhash(dbl);
		break;
	case TYPE_lng:
		finishhash(lng);
		break;
#ifdef HAVE_HGE
	case TYPE_hge:
		finishhash(hge);
		break;
#endif
	default:
		if (cand) {
			for (; p < cnt; p++) {
				const void *restrict v = BUNtail(bi, cand[p] - b->hseqbase);
				c = heap_hash_any(b->tvheap, h, v);
				hget = HASHget(h, c);
				nslots += hget == hnil;
				HASHputlink(h, p, hget);
				HASHput(h, c, p);
			}
		} else {
			for (; p < cnt; p++) {
				const void *restrict v = BUNtail(bi, p + start);
				c = heap_hash_any(b->tvheap, h, v);
				hget = HASHget(h, c);
				nslots += hget == hnil;
				HASHputlink(h, p, hget);
				HASHput(h, c, p);
			}
		}
		break;
	}
	((size_t *) h->heap.base)[5] = (size_t) nslots;
#ifndef NDEBUG
	/* clear unused part of Link array */
	memset((char *) h->Link + cnt * h->width, 0, (h->lim - cnt) * h->width);
#endif
	h->heap.parentid = b->batCacheid;
	/* if the number of occupied slots is equal to the bat count,
	 * all values are necessarily distinct */
	if (nslots == BATcount(b) && !b->tkey) {
		b->tkey = true;
		b->batDirtydesc = true;
	}
	ALGODEBUG {
		fprintf(stderr, "#BAThash: hash construction " LLFMT " usec\n", GDKusec() - t0);
		HASHcollisions(b, h);
	}
	return h;
}

gdk_return
BAThash(BAT *b, BUN masksize)
{
	assert(b->batCacheid > 0);
	if (BATcheckhash(b)) {
		return GDK_SUCCEED;
	}
	MT_lock_set(&GDKhashLock(b->batCacheid));
	if (b->thash == NULL) {
		if ((b->thash = BAThash_impl(b, NULL, masksize, "thash")) == NULL) {
			MT_lock_unset(&GDKhashLock(b->batCacheid));
			return GDK_FAIL;
		}
#ifdef PERSISTENTHASH
		if (BBP_status(b->batCacheid) & BBPEXISTING && !b->theap.dirty) {
			MT_Id tid;
			BBPfix(b->batCacheid);
			if (MT_create_thread(&tid, BAThashsync, b,
					     MT_THR_DETACHED) < 0) {
				/* couldn't start thread: clean up */
				BBPunfix(b->batCacheid);
			}
		} else
			ALGODEBUG fprintf(stderr, "#BAThash: NOT persisting hash %d\n", b->batCacheid);
#endif
	}
	MT_lock_unset(&GDKhashLock(b->batCacheid));
	return GDK_SUCCEED;
}

/*
 * The entry on which a value hashes can be calculated with the
 * routine HASHprobe.
 */
BUN
HASHprobe(const Hash *h, const void *v)
{
	switch (ATOMbasetype(h->type)) {
	case TYPE_bte:
		return hash_bte(h, v);
	case TYPE_sht:
		return hash_sht(h, v);
	case TYPE_int:
	case TYPE_flt:
		return hash_int(h, v);
	case TYPE_dbl:
	case TYPE_lng:
		return hash_lng(h, v);
#ifdef HAVE_HGE
	case TYPE_hge:
		return hash_hge(h, v);
#endif
	default:
		return hash_any(h, v);
	}
}

BUN
HASHlist(Hash *h, BUN i)
{
	BUN c = 1;
	BUN j = HASHget(h, i), nil = HASHnil(h);

	if (j == nil)
		return 1;
	while ((j = HASHgetlink(h, i)) != nil) {
		c++;
		i = j;
	}
	return c;
}

void
HASHdestroy(BAT *b)
{
	if (b) {
		Hash *hs;
		MT_lock_set(&GDKhashLock(b->batCacheid));
		hs = b->thash;
		b->thash = NULL;
		MT_lock_unset(&GDKhashLock(b->batCacheid));
		if (hs == (Hash *) 1) {
			GDKunlink(BBPselectfarm(b->batRole, b->ttype, hashheap),
				  BATDIR,
				  BBP_physical(b->batCacheid),
				  "thash");
		} else if (hs) {
			bat p = VIEWtparent(b);
			BAT *hp = NULL;

			if (p)
				hp = BBP_cache(p);

			if ((!hp || hs != hp->thash) && hs != (Hash *) -1) {
				ALGODEBUG if (*(size_t *) hs->heap.base & (1 << 24))
					fprintf(stderr, "#HASHdestroy: removing persisted hash %d\n", b->batCacheid);
				HEAPfree(&hs->heap, 1);
				GDKfree(hs);
			}
		}
	}
}

void
HASHfree(BAT *b)
{
	int err = 0;
	if (b) {
		MT_lock_set(&GDKhashLock(b->batCacheid));
		if (b->thash && b->thash != (Hash *) -1) {
			if (b->thash != (Hash *) 1) {
				if (b->thash->heap.storage == STORE_MEM &&
				    b->thash->heap.dirty) {
					if (GDKsave(b->thash->heap.farmid,
						    b->thash->heap.filename,
						    NULL,
						    b->thash->heap.base,
						    b->thash->heap.free,
						    STORE_MEM,
						    FALSE) != GDK_SUCCEED) {
						/* if saving failed, remove */
						GDKunlink(BBPselectfarm(b->batRole, b->ttype, hashheap),
							  BATDIR,
							  BBP_physical(b->batCacheid),
							  "thash");
						err = 1;
					}
					b->thash->heap.dirty = FALSE;
				}
				HEAPfree(&b->thash->heap, 0);
				GDKfree(b->thash);
				b->thash = err ? NULL : (Hash *) 1;
			}
		} else {
			b->thash = NULL;
		}
		MT_lock_unset(&GDKhashLock(b->batCacheid));
	}
}

bool
HASHgonebad(BAT *b, const void *v)
{
	Hash *h = b->thash;
	BATiter bi = bat_iterator(b);
	BUN cnt, hit;

	if (h == NULL)
		return true;	/* no hash is bad hash? */

	if (h->mask * 2 < BATcount(b)) {
		int (*cmp) (const void *, const void *) = ATOMcompare(b->ttype);
		BUN i = HASHget(h, (BUN) HASHprobe(h, v)), nil = HASHnil(h);
		for (cnt = hit = 1; i != nil; i = HASHgetlink(h, i), cnt++)
			hit += ((*cmp) (v, BUNtail(bi, (BUN) i)) == 0);

		if (cnt / hit > 4)
			return true;	/* linked list too long */

		/* in this case, linked lists are long but contain the
		 * desired values such hash tables may be useful for
		 * locating all duplicates */
	}
	return false;		/* a-ok */
}
