/*
 * lws Generic Metrics
 *
 * Copyright (C) 2019 - 2021 Andy Green <andy@warmcat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "private-lib-core.h"
#include <assert.h>

static void
lws_metrics_periodic_cb(lws_sorted_usec_list_t *sul)
{
	lws_metric_policy_dyn_t *dmp = lws_container_of(sul,
						lws_metric_policy_dyn_t, sul);
	struct lws_context *ctx = lws_container_of(dmp->list.owner,
					struct lws_context, owner_mtr_dynpol);

	if (!ctx->system_ops || !ctx->system_ops->metric_report)
		return;

	lws_start_foreach_dll(struct lws_dll2 *, d, dmp->owner.head) {
		lws_metric_t *mt = lws_container_of(d, lws_metric_t, list);
		lws_metric_pub_t *pub = priv_to_pub(mt);

		if (pub->us_first && pub->us_first != pub->us_dumped) {
			ctx->system_ops->metric_report(pub);
			pub->us_first = pub->us_dumped = lws_now_usecs();
			pub->us_last = 0;
		}

	} lws_end_foreach_dll(d);
}

/*
 * head may be the start of a linked-list of const metric policy objects or
 * just the one.
 *
 * Because we can update the device policy at runtime, we have to take care
 * about metrics object created before the policy they want becomes available,
 * or those that lose their policy when we switch out the policies and have to
 * rebind when they see the new version of the one they want fly by.
 */

int
lws_metrics_policy_dyn_create(struct lws_context *ctx,
			      const lws_metric_policy_t *head)
{
	lws_metric_policy_dyn_t *dmet;

	while (head) {
		dmet = lws_zalloc(sizeof(*dmet), __func__);

		if (!dmet)
			return 1;

		dmet->policy = head;
		lws_dll2_add_tail(&dmet->list, &ctx->owner_mtr_dynpol);

		if (head->us_schedule)
			lws_sul_schedule(ctx, 0, &dmet->sul,
					 lws_metrics_periodic_cb,
					 head->us_schedule);

		head = head->next;
	}

	return 0;
}

/*
 * Get a dynamic metrics policy by name
 */

lws_metric_policy_dyn_t *
lws_metrics_policy_by_name(struct lws_context *ctx, const char *name)
{
	lws_start_foreach_dll(struct lws_dll2 *, d, ctx->owner_mtr_dynpol.head) {
		lws_metric_policy_dyn_t *dm =
			lws_container_of(d, lws_metric_policy_dyn_t, list);

		if (!strcmp(dm->policy->name, name))
			return dm;

	} lws_end_foreach_dll(d);

	return NULL;
}

/*
 * Create a lws_metric_t, bind to a named policy if possible (or add to the
 * context list of unbound metrics) and set its lws_system
 * idx.  The metrics objects themselves are typically composed into other
 * objects and are well-known composed members of them.
 */

lws_metric_t *
lws_metric_create(struct lws_context *ctx, uint8_t flags, const char *name)
{
	// lws_metric_policy_dyn_t *dmp;
	size_t nl = strlen(name);
	lws_metric_pub_t *pub;
	lws_metric_t *mt;

	mt = (lws_metric_t *)lws_zalloc(sizeof(*mt) /* private */ +
					sizeof(lws_metric_pub_t) +
					nl + 1 /* copy of metric name */,
					__func__);
	if (!mt)
		return NULL;

	pub = priv_to_pub(mt);
	pub->name = (char *)pub + sizeof(lws_metric_pub_t);
	memcpy((char *)pub->name, name, nl + 1);
	pub->flags = flags;

	/* after these common members, we have to use the right type */

	if (!(flags & LWSMTFL_REPORT_HIST)) {
		pub->u.agg.min = ~(u_mt_t)0; /* anything is smaller or equal to this */
		pub->us_first = lws_now_usecs();
	}

	mt->ctx = ctx;
#if 0
	dmp = lws_metrics_policy_by_name(ctx, polname);
	if (dmp) {
		lwsl_notice("%s: metpol %s\n", __func__, polname);
		lws_dll2_add_tail(&mt->list, &dmp->owner);

		return 0;
	}

	lwsl_notice("%s: no metpol %s\n", __func__, polname);
#endif
	lws_dll2_add_tail(&mt->list, &ctx->owner_mtr_no_pol);

	lwsl_debug("%s: created %s\n", __func__, name);

	return mt;
}

int
lws_metric_destroy(lws_metric_t *mt, int keep)
{
	lws_metric_pub_t *pub = priv_to_pub(mt);
	lws_dll2_remove(&mt->list);

	if (keep) {
		lws_dll2_add_tail(&mt->list, &mt->ctx->owner_mtr_no_pol);

		return 0;
	}

	if (pub->flags & LWSMTFL_REPORT_HIST) {
		lws_metric_bucket_t *b = pub->u.hist.head, *b1;

		pub->u.hist.head = NULL;

		while (b) {
			b1 = b->next;
			lws_free(b);
			b = b1;
		}
	}

	lws_free(mt);

	return 0;
}

/*
 * Allow an existing metric to have its reporting policy changed at runtime
 */

int
lws_metric_switch_policy(lws_metric_t *mt, const char *polname)
{
	lws_metric_policy_dyn_t *dmp = lws_metrics_policy_by_name(mt->ctx, polname);

	lws_dll2_remove(&mt->list);
	lws_dll2_add_tail(&mt->list, &dmp->owner);

	return 0;
}

/*
 * If keep is set, don't destroy existing metrics objects, just detach them
 * from the policy being deleted and keep track of them on ctx->
 * owner_mtr_no_pol
 */

void
lws_metric_policy_dyn_destroy(lws_metric_policy_dyn_t *dm, int keep)
{
	lws_sul_cancel(&dm->sul);

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1, dm->owner.head) {
		lws_metric_t *m = lws_container_of(d, lws_metric_t, list);

		lws_metric_destroy(m, keep);

	} lws_end_foreach_dll_safe(d, d1);

	lws_dll2_remove(&dm->list);
	lws_free(dm);
}

/*
 * Destroy all dynamic metrics policies, deinit any metrics still using them
 */

void
lws_metrics_destroy(struct lws_context *ctx)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   ctx->owner_mtr_dynpol.head) {
		lws_metric_policy_dyn_t *dm =
			lws_container_of(d, lws_metric_policy_dyn_t, list);

		lws_metric_policy_dyn_destroy(dm, 0); /* don't keep */

	} lws_end_foreach_dll_safe(d, d1);

	/* destroy metrics with no current policy too... */

	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   ctx->owner_mtr_no_pol.head) {
		lws_metric_t *mt = lws_container_of(d, lws_metric_t, list);

		lws_metric_destroy(mt, 0); /* don't keep */

	} lws_end_foreach_dll_safe(d, d1);

	/* ... that's the whole allocated metrics footprint gone... */
}

int
lws_metrics_hist_bump_(lws_metric_pub_t *pub, const char *name)
{
	lws_metric_bucket_t *buck = pub->u.hist.head;
	size_t nl = strlen(name);
	char *nm;

	assert(pub->flags & LWSMTFL_REPORT_HIST);
	assert(nl < 255);

	while (buck) {
		if (lws_metric_bucket_name_len(buck) == nl &&
		    !strcmp(name, lws_metric_bucket_name(buck))) {
			buck->count++;
			goto happy;
		}
		buck = buck->next;
	}

	buck = lws_malloc(sizeof(*buck) + nl + 2, __func__);
	if (!buck)
		return 1;

	nm = (char *)buck + sizeof(*buck);
	/* length byte at beginning of name, avoid struct alignment overhead */
	*nm = (char)nl;
	memcpy(nm + 1, name, nl + 1);

	buck->next = pub->u.hist.head;
	pub->u.hist.head = buck;
	buck->count = 1;
	pub->u.hist.list_size++;

happy:
	pub->u.hist.total_count++;

	return 0;
}

#if defined(_DEBUG)
void
lws_metrics_dump(struct lws_context *ctx)
{
	lws_start_foreach_dll_safe(struct lws_dll2 *, d, d1,
				   ctx->owner_mtr_no_pol.head) {
		lws_metric_t *mt = lws_container_of(d, lws_metric_t, list);
		lws_metric_pub_t *pub;

		pub = priv_to_pub(mt);

		if (ctx->system_ops && ctx->system_ops->metric_report)
			ctx->system_ops->metric_report(pub);

	} lws_end_foreach_dll_safe(d, d1);
}
#endif

static int
_lws_metrics_format(lws_metric_pub_t *pub, lws_usec_t now, int gng,
		    char *buf, size_t len)
{
	const lws_humanize_unit_t *schema = humanize_schema_si;
	char *end = buf + len - 1, *obuf = buf;

	if (pub->flags & LWSMTFL_REPORT_DUTY_WALLCLOCK_US)
		schema = humanize_schema_us;

	if (!(pub->flags & LWSMTFL_REPORT_MEAN)) {
		/* only the sum is meaningful */
		if (pub->flags & LWSMTFL_REPORT_DUTY_WALLCLOCK_US) {

			buf += lws_humanize(buf, lws_ptr_diff_size_t(end, buf),
					    (uint64_t)pub->u.agg.sum[gng],
					    humanize_schema_us);

			buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), " / ");

			buf += lws_humanize(buf, lws_ptr_diff_size_t(end, buf),
					    (uint64_t)(now - pub->us_first),
					    humanize_schema_us);

			buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf),
					    " (%d%%)", (int)((100 * pub->u.agg.sum[gng]) /
						(unsigned long)(now - pub->us_first)));
		} else {
			/* it's a monotonic ordinal, like total tx */
			buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), "(%u) ",
					(unsigned int)pub->u.agg.count[gng]);
			buf += lws_humanize(buf, lws_ptr_diff_size_t(end, buf),
					    (uint64_t)pub->u.agg.sum[gng],
					    humanize_schema_si);
		}

	} else {
		buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), "%u, mean: ", (unsigned int)pub->u.agg.count[gng]);
		/* the average over the period is meaningful */
		buf += lws_humanize(buf, lws_ptr_diff_size_t(end, buf),
				    (uint64_t)(pub->u.agg.count[gng] ?
					 pub->u.agg.sum[gng] / pub->u.agg.count[gng] : 0),
				    schema);
	}

	return lws_ptr_diff(buf, obuf);
}

int
lws_metrics_format(lws_metric_pub_t *pub, char *buf, size_t len)
{
	char *end = buf + len - 1, *obuf = buf;
	lws_usec_t t = lws_now_usecs();
	const lws_humanize_unit_t *schema = humanize_schema_si;

	if (pub->flags & LWSMTFL_REPORT_DUTY_WALLCLOCK_US)
		schema = humanize_schema_us;

	buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), "%s: ",
				pub->name);

	if (pub->flags & LWSMTFL_REPORT_HIST) {
		lws_metric_bucket_t *buck = pub->u.hist.head;

		buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf),
				    "tot: %llu, [ ",
				    (unsigned long long)pub->u.hist.total_count);

		while (buck) {
			buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf),
					    "%s: %llu",
					    lws_metric_bucket_name(buck),
					    (unsigned long long)buck->count);
			if (buck->next && lws_ptr_diff_size_t(end, buf) > 3) {
				*buf++ = ',';
				*buf++ = ' ';
			}

			buck = buck->next;
		}

		buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), " ]");

		goto happy;
	}

	if (!pub->u.agg.count[METRES_GO] && !pub->u.agg.count[METRES_NOGO])
		return 0;

	if (pub->u.agg.count[METRES_GO]) {
		if (!(pub->flags & LWSMTFL_REPORT_ONLY_GO))
			buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), "Go: ");
		buf += _lws_metrics_format(pub, t, METRES_GO, buf, lws_ptr_diff_size_t(end, buf));
	}

	if (!(pub->flags & LWSMTFL_REPORT_ONLY_GO) && pub->u.agg.count[METRES_NOGO]) {
		buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), ", NoGo: ");
		buf += _lws_metrics_format(pub, t, METRES_NOGO, buf, lws_ptr_diff_size_t(end, buf));
	}

	if (pub->flags & LWSMTFL_REPORT_MEAN) {
		buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), ", min: ");
		buf += lws_humanize(buf, lws_ptr_diff_size_t(end, buf), pub->u.agg.min,
				    schema);
		buf += lws_snprintf(buf, lws_ptr_diff_size_t(end, buf), ", max: ");
		buf += lws_humanize(buf, lws_ptr_diff_size_t(end, buf), pub->u.agg.max,
				    schema);
	}

happy:
	return lws_ptr_diff(buf, obuf);
}

/*
 * We want to, at least internally, record an event... depending on the policy,
 * that might cause us to call through to the lws_system apis, or just update
 * our local stats about it and dump at the next periodic chance (also set by
 * the policy)
 */

void
lws_metric_event(lws_metric_t *mt, char go_nogo, u_mt_t val)
{
	//const lws_metric_policy_t *policy;
	//lws_metric_policy_dyn_t *mdp;
	lws_metric_pub_t *pub;
	//struct lws_context *ctx;

	assert((go_nogo & 0xfe) == 0);

	if (!mt)
		return;

	//ctx = mt->ctx;

	pub = priv_to_pub(mt);

	pub->us_last = lws_now_usecs();
	if (!pub->us_first)
		pub->us_first = pub->us_last;
	pub->u.agg.count[(int)go_nogo]++;
	pub->u.agg.sum[(int)go_nogo] += val;
	if (val > pub->u.agg.max)
		pub->u.agg.max = val;
	if (val < pub->u.agg.min)
		pub->u.agg.min = val;

#if 0

	if (o == &ctx->owner_mtr_no_pol) {
		/* our policy currently isn't available */
		//lwsl_warn("%s: no policy %s\n", __func__, mt->polname);

		return;
	}

	mdp = lws_container_of(o, lws_metric_policy_dyn_t, owner);
	policy = mdp->policy;
	assert(policy);

	//if (policy->flags)
#endif
#if 0
	if (ctx->system_ops->metric_report)
		ctx->system_ops->metric_report(&pub->pub);
#endif
}
