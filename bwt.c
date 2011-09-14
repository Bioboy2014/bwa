/* The MIT License

   Copyright (c) 2008 Genome Research Ltd (GRL).

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/* Contact: Heng Li <lh3@sanger.ac.uk> */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include "utils.h"
#include "bwt.h"

static const uint64_t occ_mask[32] = {
	0xc0000000ull, 	0xf0000000ull, 	0xfc000000ull,
	0xff000000ull, 	0xffc00000ull, 	0xfff00000ull,
	0xfffc0000ull, 	0xffff0000ull, 	0xffffc000ull,
	0xfffff000ull, 	0xfffffc00ull, 	0xffffff00ull,
	0xffffffc0ull, 	0xfffffff0ull, 	0xfffffffcull,
	0xffffffffull, 	0xc0000000ffffffffull, 	0xf0000000ffffffffull,
	0xfc000000ffffffffull, 	0xff000000ffffffffull, 	0xffc00000ffffffffull,
	0xfff00000ffffffffull, 	0xfffc0000ffffffffull, 	0xffff0000ffffffffull,
	0xffffc000ffffffffull, 	0xfffff000ffffffffull, 	0xfffffc00ffffffffull,
	0xffffff00ffffffffull, 	0xffffffc0ffffffffull, 	0xfffffff0ffffffffull,
	0xfffffffcffffffffull, 0xffffffffffffffffull
};
static const uint64_t n_mask[5] = { 0xffffffffffffffffull, 0xaaaaaaaaaaaaaaaaull, 
		0x5555555555555555ull, 0x0ull, 0xffffffffffffffffull };

void bwt_gen_cnt_table(bwt_t *bwt)
{
	int i, j;
	for (i = 0; i != 256; ++i) {
		uint32_t x = 0;
		for (j = 0; j != 4; ++j)
			x |= (((i&3) == j) + ((i>>2&3) == j) + ((i>>4&3) == j) + (i>>6 == j)) << (j<<3);
		bwt->cnt_table[i] = x;
	}
}

static inline int __occ_aux(uint64_t y)
{
	// reduce nucleotide counting to bits counting
	y = (y >> 1) & y;
	// count the number of 1s in y
	y = (y & 0x1111111111111111ull) + (y >> 2 & 0x1111111111111111ull);
	return ((y + (y >> 4)) & 0xf0f0f0f0f0f0f0full) * 0x101010101010101ull >> 56;
}

static inline bwtint_t bwt_occ_0(bwtint_t l, const uint64_t *p)
{
	bwtint_t n = 0;
	switch (l) {
		case 3: n += __occ_aux(~*(--p));
		case 2: n += __occ_aux(~*(--p));
		default: return n + __occ_aux(~*(--p));
	}
}

static inline bwtint_t bwt_occ_x(const uint64_t x, bwtint_t l, const uint64_t *p)
{
	bwtint_t n = 0;
	switch (l) {
		case 3: n += __occ_aux(*(--p) ^ x);
		case 2: n += __occ_aux(*(--p) ^ x);
		default: return n + __occ_aux(*(--p) ^ x);
	}
}

static inline bwtint_t bwt_occ_3(bwtint_t l, const uint64_t *p)
{
	bwtint_t n = 0;
	switch (l) {
		case 3: n += __occ_aux(*(--p));
		case 2: n += __occ_aux(*(--p));
		default: return n + __occ_aux(*(--p));
	}
}

static inline bwtint_t bwt_occ(const uint64_t *p, const bwtint_t ko, bwtint_t k, const ubyte_t c)
{
	bwtint_t n, l;
	p += ko * 6;
	n = ((uint32_t *)p)[c];
	k -= ko * OCC_INTERVAL;
	l = k >> 5;
	p += 2; // jump to the start of the first BWT cell

	// calculate Occ up to the last k/32
	switch (c) {
		case 0: k &= 31;
			if (l) n += bwt_occ_0(l, p += l);
			n += __occ_aux(~(*p & occ_mask[k])) - (k^31);
			break;
		case 3: if (l) n += bwt_occ_3(l, p += l);
			n += __occ_aux(*p & occ_mask[k&31]);
			break;
		default: if (l) n += bwt_occ_x(n_mask[c], l, p += l);
			n += __occ_aux((*p & occ_mask[k&31]) ^ n_mask[c]);
	}
	return n;
}

static inline bwtint_t cal_isa(const bwt_t *bwt, bwtint_t isa)
{
	bwtint_t c, _isa, isa_o;
	if (isa != bwt->primary) {
		_isa = (isa < bwt->primary) ? isa : isa - 1;
		isa_o = _isa/OCC_INTERVAL;
		c = bwt_B0(bwt, _isa, isa_o);
		if (isa < bwt->seq_len) {
			isa = bwt->L2[c] + bwt_occ((uint64_t *)bwt->bwt, isa_o, _isa, c);
		} else {
			isa = (isa == bwt->seq_len ? bwt->L2[c+1] : bwt->L2[c]);
		}
	} else {
		isa = 0;
	}
	return isa;
}

// bwt->bwt and bwt->occ must be precalculated
void bwt_cal_sa(bwt_t *bwt, int intv)
{
	bwtint_t isa, sa, i; // S(isa) = sa

	xassert(bwt->bwt, "bwt_t::bwt is not initialized.");

	if (bwt->sa) free(bwt->sa);
	bwt->sa_intv = intv;
	bwt->n_sa = (bwt->seq_len + intv) / intv;
	bwt->sa = (bwtint_t*)calloc(bwt->n_sa, sizeof(bwtint_t));
	// calculate SA value
	isa = 0; sa = bwt->seq_len;
	for (i = 0; i < bwt->seq_len; ++i) {
		if (isa % intv == 0) bwt->sa[isa/intv] = sa;
		--sa;
		isa = cal_isa(bwt, isa);
	}
	if (isa % intv == 0) bwt->sa[isa/intv] = sa;
	bwt->sa[0] = (bwtint_t)-1; // before this line, bwt->sa[0] = bwt->seq_len
}

bwtint_t bwt_sa(const bwt_t *bwt, bwtint_t k)
{
	bwtint_t m, add, sa = 0;
	m = bwt->sa_intv - 1;
	if ((m+1) & m) { // not power of 2 before decrement
		add = m;
		m |= m>>1;
		m |= m>>2;
		m |= m>>4;
		m |= m>>8;
		m |= m>>16;
		add ^= m;
	} else {
		add = 0;
	}
	while (((k + add) & m)) {
		++sa;
		k = cal_isa(bwt, k);
	}
	/* without setting bwt->sa[0] = -1, the following line should be
	   changed to (sa + bwt->sa[k/bwt->sa_intv]) % (bwt->seq_len + 1) */
	return sa + bwt->sa[k/bwt->sa_intv];
}

// an analogy to bwt_occ() but more efficient, requiring k <= l
inline void bwt_2occ(const bwt_t *bwt, bwtint_t *k, bwtint_t *l, ubyte_t c)
{
	bwtint_t i, m, n, lo;
	const uint64_t *p;
	n = *k - 1;
	m = (n >= bwt->primary)? n-1 : n;
	i = (*l >= bwt->primary)? *l-1 : *l;
	n = m/OCC_INTERVAL;
	lo = i/OCC_INTERVAL;
	if (lo != n || *k == 0 || i == m) {
		if (*k != 0) {
			*k = bwt->L2[c] + bwt_occ((const uint64_t *)bwt->bwt, n, m, c) + 1;
			if (i != m) {
				*l = bwt->L2[c] +
					bwt_occ((const uint64_t *)bwt->bwt, lo, i, c);
			} else {
				*l = *k;
			}
		} else {
			*k = bwt->L2[c] + 1;
			*l = bwt->L2[c+1];
		}
	} else {
		p = (const uint64_t *)bwt->bwt + n * 6;
		*k = m & 31;
		m = ((m - (n * OCC_INTERVAL)) >> 5); //k-len
		*l = i & 31;
		i = ((i - (lo * OCC_INTERVAL)) >> 5); //total len
		n = ((uint32_t *)p)[c] + bwt->L2[c];
		p += 2 + i; // jump to the end of the last BWT cell
		i = i - m; //total len - k len = l-len
		switch (c) {
		case 0: *l = __occ_aux(~(*p & occ_mask[*l])) - (*l^31);
			if (i) { // these (i) tests aren't always true (?)
				*l += bwt_occ_0(i, p);
				p -= i; //yes, postdecr: reverse iteration
			}
			*k = __occ_aux(~(*p & occ_mask[*k])) - (*k^31) + 1;
			if (*k <= *l) {
				if (m) n += bwt_occ_0(m, p);
				*l += n;
				*k += n ;
			}
			break;
		case 3: *l = __occ_aux(*p & occ_mask[*l]);
			if (i) {
				*l += bwt_occ_3(i, p);
				p -= i;
			}
			*k = __occ_aux(*p & occ_mask[*k]) + 1;
			if (*k <= *l) {
				if (m) n += bwt_occ_3(m, p);
				*l += n;
				*k += n;
			}
			break;
		default: *l = __occ_aux((*p & occ_mask[*l]) ^ n_mask[c]);
			if (i) {
				*l += bwt_occ_x(n_mask[c], i, p);
				p -= i;
			}
			*k = __occ_aux((*p & occ_mask[*k]) ^ n_mask[c]) + 1;
			if (*k <= *l) {
				if (m) n += bwt_occ_x(n_mask[c], m, p);
				*l += n;
				*k += n;
			}
		}
	}
}

#define __occ_aux4(bwt, b)											\
	((bwt)->cnt_table[(b)&0xff] + (bwt)->cnt_table[(b)>>8&0xff]		\
	 + (bwt)->cnt_table[(b)>>16&0xff] + (bwt)->cnt_table[(b)>>24])

inline void bwt_occ4(const bwt_t *bwt, bwtint_t k, bwtint_t cnt[4])
{
	bwtint_t l, j, x;
	uint32_t *p;
	if (k == (bwtint_t)(-1)) {
		memset(cnt, 0, 4 * sizeof(bwtint_t));
		return;
	}
	if (k >= bwt->primary) --k; // because $ is not in bwt
	p = bwt_occ_intv(bwt, k);
	memcpy(cnt, p, 16);
	p += 4;
	j = k >> 4 << 4;
	for (l = k / OCC_INTERVAL * OCC_INTERVAL, x = 0; l < j; l += 16, ++p)
		x += __occ_aux4(bwt, *p);
	x += __occ_aux4(bwt, *p & occ_mask[k&15]) - (~k&15);
	cnt[0] += x&0xff; cnt[1] += x>>8&0xff; cnt[2] += x>>16&0xff; cnt[3] += x>>24;
}

// an analogy to bwt_occ4() but more efficient, requiring k <= l
inline void bwt_2occ4(const bwt_t *bwt, bwtint_t k, bwtint_t l, bwtint_t cntk[4], bwtint_t cntl[4])
{
	bwtint_t _k, _l;
	if (k == l) {
		bwt_occ4(bwt, k, cntk);
		memcpy(cntl, cntk, 4 * sizeof(bwtint_t));
		return;
	}
	_k = (k >= bwt->primary)? k-1 : k;
	_l = (l >= bwt->primary)? l-1 : l;
	if (_l/OCC_INTERVAL != _k/OCC_INTERVAL || k == (bwtint_t)(-1) || l == (bwtint_t)(-1)) {
		bwt_occ4(bwt, k, cntk);
		bwt_occ4(bwt, l, cntl);
	} else {
		bwtint_t i, j, x, y;
		uint32_t *p;
		int cl[4];
		if (k >= bwt->primary) --k; // because $ is not in bwt
		if (l >= bwt->primary) --l;
		cl[0] = cl[1] = cl[2] = cl[3] = 0;
		p = bwt_occ_intv(bwt, k);
		memcpy(cntk, p, 4 * sizeof(bwtint_t));
		p += 4;
		// prepare cntk[]
		j = k >> 4 << 4;
		for (i = k / OCC_INTERVAL * OCC_INTERVAL, x = 0; i < j; i += 16, ++p)
			x += __occ_aux4(bwt, *p);
		y = x;
		x += __occ_aux4(bwt, *p & occ_mask[k&15]) - (~k&15);
		// calculate cntl[] and finalize cntk[]
		j = l >> 4 << 4;
		for (; i < j; i += 16, ++p)
			y += __occ_aux4(bwt, *p);
		y += __occ_aux4(bwt, *p & occ_mask[l&15]) - (~l&15);
		memcpy(cntl, cntk, 16);
		cntk[0] += x&0xff; cntk[1] += x>>8&0xff; cntk[2] += x>>16&0xff; cntk[3] += x>>24;
		cntl[0] += y&0xff; cntl[1] += y>>8&0xff; cntl[2] += y>>16&0xff; cntl[3] += y>>24;
	}
}

int bwt_match_exact(const bwt_t *bwt, int len, const ubyte_t *str, bwtint_t *sa_begin, bwtint_t *sa_end)
{
	bwtint_t k, l;
	int i;
	k = 0; l = bwt->seq_len;
	for (i = len - 1; i >= 0; --i) {
		ubyte_t c = str[i];
		if (c > 3) return 0; // no match
		bwt_2occ(bwt, &k, &l, c);
		if (k > l) break; // no match
	}
	if (k > l) return 0; // no match
	if (sa_begin) *sa_begin = k;
	if (sa_end)   *sa_end = l;
	return l - k + 1;
}

int bwt_match_exact_alt(const bwt_t *bwt, int len, const ubyte_t *str, bwtint_t *k0, bwtint_t *l0)
{
	int i;
	bwtint_t k, l;
	k = *k0; l = *l0;
	for (i = len - 1; i >= 0; --i) {
		ubyte_t c = str[i];
		if (c > 3) return 0; // there is an N here. no match
		bwt_2occ(bwt, &k, &l, c);
		if (k > l) return 0; // no match
	}
	*k0 = k; *l0 = l;
	return l - k + 1;
}
