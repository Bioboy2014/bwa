#include "bwa.h"
#include "bwamem.h"
#include "bntseq.h"
#include "kstring.h"

/***************************
 * SMEM iterator interface *
 ***************************/

struct __smem_i {
	const bwt_t *bwt;
	const uint8_t *query;
	int start, len;
	bwtintv_v *matches; // matches; to be returned by smem_next()
	bwtintv_v *sub;     // sub-matches inside the longest match; temporary
	bwtintv_v *tmpvec[2]; // temporary arrays
};

smem_i *smem_itr_init(const bwt_t *bwt)
{
	smem_i *itr;
	itr = calloc(1, sizeof(smem_i));
	itr->bwt = bwt;
	itr->tmpvec[0] = calloc(1, sizeof(bwtintv_v));
	itr->tmpvec[1] = calloc(1, sizeof(bwtintv_v));
	itr->matches   = calloc(1, sizeof(bwtintv_v));
	itr->sub       = calloc(1, sizeof(bwtintv_v));
	return itr;
}

void smem_itr_destroy(smem_i *itr)
{
	free(itr->tmpvec[0]->a); free(itr->tmpvec[0]);
	free(itr->tmpvec[1]->a); free(itr->tmpvec[1]);
	free(itr->matches->a);   free(itr->matches);
	free(itr->sub->a);       free(itr->sub);
	free(itr);
}

void smem_set_query(smem_i *itr, int len, const uint8_t *query)
{
	itr->query = query;
	itr->start = 0;
	itr->len = len;
}

const bwtintv_v *smem_next(smem_i *itr)
{
	int i, max, max_i, ori_start;
	itr->tmpvec[0]->n = itr->tmpvec[1]->n = itr->matches->n = itr->sub->n = 0;
	if (itr->start >= itr->len || itr->start < 0) return 0;
	while (itr->start < itr->len && itr->query[itr->start] > 3) ++itr->start; // skip ambiguous bases
	if (itr->start == itr->len) return 0;
	ori_start = itr->start;
	itr->start = bwt_smem1(itr->bwt, itr->len, itr->query, ori_start, 1, itr->matches, itr->tmpvec); // search for SMEM
	if (itr->matches->n == 0) return itr->matches; // well, in theory, we should never come here
	for (i = max = 0, max_i = 0; i < itr->matches->n; ++i) { // look for the longest match
		bwtintv_t *p = &itr->matches->a[i];
		int len = (uint32_t)p->info - (p->info>>32);
		if (max < len) max = len, max_i = i;
	}
	return itr->matches;
}

/***********************
 *** Extra functions ***
 ***********************/

mem_alnreg_v mem_align1(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int l_seq, const char *seq_)
{ // the difference from mem_align1_core() is that this routine: 1) calls mem_mark_primary_se(); 2) does not modify the input sequence
	extern mem_alnreg_v mem_align1_core(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int l_seq, char *seq);
	extern void mem_mark_primary_se(const mem_opt_t *opt, int n, mem_alnreg_t *a, int64_t id);
	mem_alnreg_v ar;
	char *seq;
	seq = malloc(l_seq);
	memcpy(seq, seq_, l_seq); // makes a copy of seq_
	ar = mem_align1_core(opt, bwt, bns, pac, l_seq, seq);
	mem_mark_primary_se(opt, ar.n, ar.a, lrand48());
	free(seq);
	return ar;
}

void mem_reg2ovlp(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, bseq1_t *s, mem_alnreg_v *a)
{
	int i;
	kstring_t str = {0,0,0};
	for (i = 0; i < a->n; ++i) {
		const mem_alnreg_t *p = &a->a[i];
		int is_rev, rid, qb = p->qb, qe = p->qe;
		int64_t pos, rb = p->rb, re = p->re;
		pos = bns_depos(bns, rb < bns->l_pac? rb : re - 1, &is_rev);
		rid = bns_pos2rid(bns, pos);
		assert(rid == p->rid);
		pos -= bns->anns[rid].offset;
		kputs(s->name, &str); kputc('\t', &str);
		kputw(s->l_seq, &str); kputc('\t', &str);
		if (is_rev) qb ^= qe, qe ^= qb, qb ^= qe; // swap
		kputw(qb, &str); kputc('\t', &str); kputw(qe, &str); kputc('\t', &str);
		kputs(bns->anns[rid].name, &str); kputc('\t', &str);
		kputw(bns->anns[rid].len, &str); kputc('\t', &str);
		kputw(pos, &str); kputc('\t', &str); kputw(pos + (re - rb), &str); kputc('\t', &str);
		ksprintf(&str, "%.3f", (double)p->truesc / opt->a / (qe - qb > re - rb? qe - qb : re - rb));
		kputc('\n', &str);
	}
	s->sam = str.s;
}

// Okay, returning strings is bad, but this has happened a lot elsewhere. If I have time, I need serious code cleanup.
char **mem_gen_alt(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_alnreg_v *a, int l_query, const char *query) // ONLY work after mem_mark_primary_se()
{
	int i, k, *cnt, tot;
	kstring_t *aln = 0;
	char **XA = 0;

	cnt = calloc(a->n, sizeof(int));
	for (i = 0, tot = 0; i < a->n; ++i) {
		int j = a->a[i].secondary;
		if (j >= 0 && a->a[i].score >= a->a[j].score * opt->drop_ratio)
			++cnt[j], ++tot;
	}
	if (tot == 0) goto end_gen_alt;
	aln = calloc(a->n, sizeof(kstring_t));
	for (i = 0; i < a->n; ++i) {
		mem_aln_t t;
		int j = a->a[i].secondary;
		if (j < 0 || a->a[i].score < a->a[j].score * opt->drop_ratio) continue; // we don't process the primary alignments as they will be converted to SAM later
		if (cnt[j] > opt->max_hits) continue;
		t = mem_reg2aln(opt, bns, pac, l_query, query, &a->a[i]);
		kputs(bns->anns[t.rid].name, &aln[j]);
		kputc(',', &aln[j]); kputc("+-"[t.is_rev], &aln[j]); kputl(t.pos + 1, &aln[j]);
		kputc(',', &aln[j]);
		for (k = 0; k < t.n_cigar; ++k) {
			kputw(t.cigar[k]>>4, &aln[j]);
			kputc("MIDSHN"[t.cigar[k]&0xf], &aln[j]);
		}
		kputc(',', &aln[j]); kputw(t.NM, &aln[j]);
		kputc(';', &aln[j]);
		free(t.cigar);
	}
	XA = calloc(a->n, sizeof(char*));
	for (k = 0; k < a->n; ++k)
		XA[k] = aln[k].s;

end_gen_alt:
	free(cnt); free(aln);
	return XA;
}
