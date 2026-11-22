/*
 * test/engine/oracle.c
 *	  Brute-force inference by enumerating every possible world.
 */
#include "oracle.h"

#include <math.h>
#include <string.h>

double
oracle_joint(const gx_bn *bn, const int *assign)
{
	double		p = 1.0;

	for (int i = 0; i < bn->ncpts; i++)
	{
		const gx_cpt *c = &bn->cpts[i];
		size_t		idx = 0;

		for (int j = 0; j < c->nparents; j++)
			idx = idx * bn->card[c->parents[j]] + assign[c->parents[j]];
		idx = idx * bn->card[c->child] + assign[c->child];
		p *= c->lscale ? exp(c->lscale[assign[c->child]] + c->vals[idx]) : c->vals[idx];
	}
	return p;
}

int
oracle_run(const gx_bn *bn, oracle_out *out)
{
	int			missing[ORACLE_MAX_MISSING];
	int			nmissing = 0;
	int			assign[64];

	memset(out, 0, sizeof(*out));
	for (int v = 0; v < bn->nvars; v++)
	{
		if (bn->evidence[v] >= 0)
			assign[v] = bn->evidence[v];
		else
		{
			if (nmissing == ORACLE_MAX_MISSING)
				return -1;
			missing[nmissing++] = v;
			assign[v] = 0;
		}
	}

	/* Odometer over the missing variables. */
	out->map_joint = -1.0;
	for (;;)
	{
		double		p = oracle_joint(bn, assign);
		int			k;

		out->z += p;
		for (int i = 0; i < nmissing; i++)
			out->marg[missing[i]][assign[missing[i]]] += p;
		if (p > out->map_joint)
		{
			out->map_joint = p;
			memcpy(out->map, assign, sizeof(int) * bn->nvars);
		}

		for (k = 0; k < nmissing; k++)
		{
			int			v = missing[k];

			if (++assign[v] < bn->card[v])
				break;
			assign[v] = 0;
		}
		if (k == nmissing)
			break;
	}

	if (out->z > 0)
		for (int i = 0; i < nmissing; i++)
			for (int x = 0; x < bn->card[missing[i]]; x++)
				out->marg[missing[i]][x] /= out->z;
	return 0;
}

int
oracle_counts(const gx_bn *bn, const int *group, const size_t *goff,
			  double *counts, size_t ncounts, double *z)
{
	int			missing[ORACLE_MAX_MISSING];
	int			nmissing = 0;
	int			assign[64];
	*z = 0.0;
	for (int v = 0; v < bn->nvars; v++)
	{
		if (bn->evidence[v] >= 0)
			assign[v] = bn->evidence[v];
		else
		{
			if (nmissing == ORACLE_MAX_MISSING)
				return -1;
			missing[nmissing++] = v;
			assign[v] = 0;
		}
	}

	/* Every world: its joint goes to the cell each CPT's family takes. */
	for (;;)
	{
		double		p = oracle_joint(bn, assign);
		int			k;

		*z += p;
		for (int i = 0; i < bn->ncpts; i++)
		{
			const gx_cpt *c = &bn->cpts[i];
			size_t		idx = 0;

			for (int j = 0; j < c->nparents; j++)
				idx = idx * bn->card[c->parents[j]] + assign[c->parents[j]];
			idx = idx * bn->card[c->child] + assign[c->child];
			counts[goff[group[i]] + idx] += p;
		}
		for (k = 0; k < nmissing; k++)
		{
			int			v = missing[k];

			if (++assign[v] < bn->card[v])
				break;
			assign[v] = 0;
		}
		if (k == nmissing)
			break;
	}

	if (*z == 0)
		return 1;
	for (size_t x = 0; x < ncounts; x++)
		counts[x] /= *z;
	return 0;
}
