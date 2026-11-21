#!/usr/bin/env python3
"""Check gaps.learn_cpts(method => 'em') against an EM loop built on pgmpy.

Usage: test/pgmpy/em.py [--seed N] [--models N] [--iterations N]

Connects to the server named by the usual PG* environment variables, where
pg_gaps must be installable.  For each random model (check.py's
build_model: a few attributes per row, a random fraction missing, an
optional vertical factor) it:

  1. reads back the ground network pg_gaps built (gaps._ground);
  2. starts from the complete-data estimate, computed here from the
     network: counts of entities whose child and parents are observed,
     plus alpha, normalized per parent row;
  3. runs EM here: the E-step asks pgmpy's VariableElimination for each
     CPT instance's joint posterior over its missing child and parents,
     the M-step adds alpha and normalizes;
  4. runs gaps.learn_cpts(model, alpha, 'em', N, 0), which does N
     iterations unless the objective stops rising sooner, replays as
     many here, and compares every CPT entry to 1e-9 and loglik, here
     log P(evidence) by the chain rule over pgmpy queries, to 1e-8.

pgmpy's own ExpectationMaximization estimator cannot be the oracle: it
drops every row with a value missing in a partially observed column, and
only learns columns missing everywhere.  Exits non-zero on the first
disagreement, printing the model's seed.
"""

import argparse
import math
import random
import sys
import warnings
from itertools import product

import psycopg
from check import build_model
from pgmpy.factors.discrete import TabularCPD
from pgmpy.inference import VariableElimination
from pgmpy.models import DiscreteBayesianNetwork

TOL = 1e-9
LL_TOL = 1e-8


class Network:
    """The ground network of gaps._ground, 0-based, with each CPT
    instance's family and the factor (cpt_off) it belongs to."""

    def __init__(self, row):
        card, evidence, child, parent_off, parents, cpt_off, _, _ = row
        self.card = card
        self.ev = {v: evidence[v] - 1 for v in range(len(card)) if evidence[v] > 0}
        self.inst = []
        for j in range(len(child)):
            ps = [p - 1 for p in parents[parent_off[j] - 1 : parent_off[j + 1] - 1]]
            self.inst.append((child[j] - 1, ps, cpt_off[j]))
        # Factor sizes and order: cpt_off ascending is factor order.
        self.size = {}
        for c, ps, off in self.inst:
            self.size[off] = math.prod(card[v] for v in [*ps, c])
        self.offs = sorted(self.size)
        self.k = {off: card[c] for c, ps, off in self.inst}

    def cell(self, fam, vals):
        idx = 0
        for v, x in zip(fam, vals):
            idx = idx * self.card[v] + x
        return idx


def normalize(n, k):
    out = []
    for r in range(0, len(n), k):
        s = sum(n[r : r + k])
        out.extend(x / s if s > 0 else 1.0 / k for x in n[r : r + k])
    return out


def pgmpy_net(net, cpt):
    bn = DiscreteBayesianNetwork()
    bn.add_nodes_from(range(len(net.card)))
    for c, ps, _ in net.inst:
        for p in ps:
            bn.add_edge(p, c)
    for c, ps, off in net.inst:
        k = net.card[c]
        flat = cpt[off]
        nrows = len(flat) // k
        table = [[flat[r * k + x] for r in range(nrows)] for x in range(k)]
        bn.add_cpds(
            TabularCPD(
                c,
                k,
                table,
                evidence=ps or None,
                evidence_card=[net.card[p] for p in ps] or None,
            )
        )
    return VariableElimination(bn)


def loglik(net, infer):
    """log P(evidence), as P(e1) P(e2 | e1) ... over pgmpy queries."""
    total, seen = 0.0, {}
    for v, x in sorted(net.ev.items()):
        q = infer.query([v], evidence=seen or None, show_progress=False)
        total += math.log(float(q.values[x]))
        seen[v] = x
    return total


def e_step(net, cpt):
    """Expected counts per factor, and log P(evidence)."""
    infer = pgmpy_net(net, cpt)
    counts = {off: [0.0] * net.size[off] for off in net.offs}
    for c, ps, off in net.inst:
        fam = [*ps, c]
        miss = [v for v in fam if v not in net.ev]
        if miss:
            q = infer.query(
                miss, evidence=net.ev or None, joint=True, show_progress=False
            )
            order = [q.variables.index(v) for v in miss]
            for vals in product(*[range(net.card[v]) for v in miss]):
                a = dict(zip(miss, vals))
                p = float(
                    q.values[tuple(vals[order.index(i)] for i in range(len(miss)))]
                )
                full = [net.ev.get(v, a.get(v)) for v in fam]
                counts[off][net.cell(fam, full)] += p
        else:
            counts[off][net.cell(fam, [net.ev[v] for v in fam])] += 1.0
    return counts, loglik(net, infer)


def em(net, alpha, iterations):
    counts = {off: [0.0] * net.size[off] for off in net.offs}
    for c, ps, off in net.inst:
        fam = [*ps, c]
        if all(v in net.ev for v in fam):
            counts[off][net.cell(fam, [net.ev[v] for v in fam])] += 1.0
    cpt = {
        off: normalize([alpha + n for n in counts[off]], net.k[off]) for off in net.offs
    }
    for _ in range(iterations):
        counts, _ = e_step(net, cpt)
        cpt = {
            off: normalize([alpha + n for n in counts[off]], net.k[off])
            for off in net.offs
        }
    return cpt, e_step(net, cpt)[1]


def check_model(cur, name, seed, alpha, iterations):
    cur.execute("SELECT * FROM gaps._ground(gaps._model_id(%s))", [name])
    net = Network(cur.fetchone())
    cur.execute(
        "SELECT max(iterations), max(loglik) FROM gaps.learn_cpts(%s, %s, 'em', %s, 0)",
        [name, alpha, iterations],
    )
    its, ours_ll = cur.fetchone()
    cur.execute(
        "SELECT cpt FROM gaps.factor WHERE model_id = gaps._model_id(%s) "
        "ORDER BY factor_id",
        [name],
    )
    ours = [row[0] for row in cur.fetchall()]
    want, want_ll = em(net, alpha, its)
    if len(ours) != len(net.offs):
        print(f"FAIL (model seed {seed}): factor count differs", file=sys.stderr)
        sys.exit(1)
    for off, got in zip(net.offs, ours):
        for x, (a, b) in enumerate(zip(got, want[off])):
            if abs(a - b) > TOL:
                print(
                    f"FAIL (model seed {seed}): factor at {off} cell {x}: "
                    f"pg_gaps {a}, oracle {b} after {its} iterations",
                    file=sys.stderr,
                )
                sys.exit(1)
    if abs(ours_ll - want_ll) > LL_TOL:
        print(
            f"FAIL (model seed {seed}): loglik pg_gaps {ours_ll}, oracle {want_ll}",
            file=sys.stderr,
        )
        sys.exit(1)
    missing_parents = any(v not in net.ev for c, ps, _ in net.inst for v in ps) and any(
        c in net.ev for c, ps, _ in net.inst if any(p not in net.ev for p in ps)
    )
    return sum(len(c) for c in ours), its, missing_parents


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--models", type=int, default=100)
    ap.add_argument("--iterations", type=int, default=4)
    args = ap.parse_args()
    warnings.filterwarnings("ignore")

    cells = models = skipped = informative = 0
    with psycopg.connect(autocommit=False) as conn:
        cur = conn.cursor()
        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_gaps")
        conn.commit()
        for i in range(args.models):
            seed = args.seed * 100003 + i
            rng = random.Random(seed)
            alpha = rng.choice([0.5, 1.0, 2.0])
            name = "pgmpy_em"
            build_model(cur, rng, name)
            try:
                n, _, inf = check_model(cur, name, seed, alpha, args.iterations)
            except psycopg.errors.DataException as e:
                if "probability zero" not in str(e):
                    raise
                skipped += 1
                conn.rollback()
                continue
            cells += n
            models += 1
            informative += inf
            conn.rollback()
    # Rule 8: the comparison must include models where EM differs from
    # counting, an observed child with a missing parent.
    if models == 0 or informative == 0:
        print(
            f"FAIL: {models} models compared, {informative} with a missing parent "
            "of an observed child",
            file=sys.stderr,
        )
        sys.exit(1)
    print(
        f"pgmpy em: {cells} CPT entries agree to {TOL} after {args.iterations} "
        f"iterations over {models} models ({informative} with a missing parent of "
        f"an observed child; {skipped} skipped: impossible evidence)"
    )


if __name__ == "__main__":
    main()
