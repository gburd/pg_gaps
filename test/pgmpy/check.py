#!/usr/bin/env python3
"""Check pg_gaps marginals against pgmpy, an independent implementation.

Usage: test/pgmpy/check.py [--seed N] [--models N]

Connects to the server named by the usual PG* environment variables, where
pg_gaps must be installable.  For each random model it:

  1. creates a table of random rows, with a random fraction of values
     missing, and declares a random first-order model over it in pg_gaps:
     a few attributes per row, each with a prior or with parents among
     earlier attributes of the same row (as a CPT, or as a logistic or
     noisy-OR factor), and optionally a vertical factor tying row r's
     attribute to row r-1's;
  2. reads back the ground network pg_gaps built (gaps._ground) and builds
     the same Bayesian network in pgmpy, with the same CPTs and evidence;
  3. asks pgmpy's VariableElimination for every missing variable's
     marginal and compares it with gaps.marginals, to 1e-9.

The ground network is read from pg_gaps on purpose: this test checks the
inference, not the grounding (sql/infer.sql checks grounding against a
hand-written possible-worlds computation).  Exits non-zero on the first
disagreement, printing the model's seed.
"""

import argparse
import json
import random
import sys
import warnings

import psycopg
from pgmpy.factors.discrete import TabularCPD
from pgmpy.inference import VariableElimination
from pgmpy.models import DiscreteBayesianNetwork

TOL = 1e-9


def random_cpt(rng, nrows, k, zero_frac):
    """nrows distributions over k values, row-major, child fastest."""
    out = []
    for _ in range(nrows):
        row = [
            0.0 if rng.random() < zero_frac else 0.05 + rng.random() for _ in range(k)
        ]
        if sum(row) == 0:
            row[rng.randrange(k)] = 1.0
        s = sum(row)
        out.extend(x / s for x in row)
    return out


def build_model(cur, rng, name):
    """Create a table and a random model over it.  Returns nothing; the
    model is read back through gaps._ground."""
    natt = rng.randint(1, 4)
    nrows = rng.randint(1, 6)
    cards = [rng.randint(2, 3) for _ in range(natt)]
    cols = ", ".join(f"a{i} text" for i in range(natt))
    cur.execute(f"DROP TABLE IF EXISTS {name}")
    cur.execute(f"CREATE TABLE {name} (r int PRIMARY KEY, {cols})")
    missing = rng.choice([0.2, 0.5, 0.8])
    for r in range(nrows):
        vals = [
            None if rng.random() < missing else f"v{rng.randrange(cards[i])}"
            for i in range(natt)
        ]
        cur.execute(f"INSERT INTO {name} VALUES (%s{', %s' * natt})", [r, *vals])

    cur.execute("SELECT gaps.create_model(%s)", [name])
    for i in range(natt):
        cur.execute(
            "SELECT gaps.declare_attribute(%s, %s, %s, %s)",
            [name, name, f"a{i}", [f"v{x}" for x in range(cards[i])]],
        )
        cur.execute(
            "SELECT gaps.add_stripe(%s, %s, %s, %s)", [name, f"s{i}", name, f"a{i}"]
        )

    # Attribute 0 may depend on the previous row's attribute 0 (a vertical
    # factor): row 0 gets a prior, rows 1.. a CPT on their predecessor.
    zero = 0.25 if rng.random() < 0.3 else 0.0
    vertical = nrows > 1 and rng.random() < 0.5
    if vertical:
        cur.execute("SELECT gaps.add_stripe(%s, 'first', %s, 'a0')", [name, name])
        cur.execute("SELECT gaps.add_stripe_qual(%s, 'first', 'r', '=', '0')", [name])
        cur.execute("SELECT gaps.add_stripe(%s, 'rest', %s, 'a0')", [name, name])
        cur.execute("SELECT gaps.add_stripe_qual(%s, 'rest', 'r', '>', '0')", [name])
        cur.execute("SELECT gaps.add_stripe(%s, 'prev', %s, 'a0')", [name, name])
        cur.execute(
            "SELECT gaps.add_stripe_qual(%s, 'prev', 'r', '<', %s)",
            [name, str(nrows - 1)],
        )
        cur.execute(
            "SELECT gaps.add_factor(%s, 'f0', 'first', '{}', %s)",
            [name, random_cpt(rng, 1, cards[0], zero)],
        )
        cur.execute(
            "SELECT gaps.add_factor(%s, 'f0v', 'rest', ARRAY['prev'], %s)",
            [name, random_cpt(rng, cards[0], cards[0], zero)],
        )
        # Map rest row r to prev row r-1 through a generated column.
        cur.execute(
            f"ALTER TABLE {name} ADD COLUMN rm1 int GENERATED ALWAYS AS (r - 1) STORED"
        )
        cur.execute("SELECT gaps.map_factor(%s, 'f0v', 1, 'rm1', 'r')", [name])
    else:
        cur.execute(
            "SELECT gaps.add_factor(%s, 'f0', 's0', '{}', %s)",
            [name, random_cpt(rng, 1, cards[0], zero)],
        )

    for i in range(1, natt):
        parents = sorted(rng.sample(range(i), rng.randint(0, min(2, i))))
        prows = 1
        for p in parents:
            prows *= cards[p]
        kind = rng.choice(["cpt", "cpt", "logistic", "noisy_or"]) if parents else "cpt"
        if kind == "noisy_or" and cards[i] != 2:
            kind = "logistic"
        if kind == "cpt":
            cur.execute(
                "SELECT gaps.add_factor(%s, %s, %s, %s, %s)",
                [
                    name,
                    f"f{i}",
                    f"s{i}",
                    [f"s{p}" for p in parents],
                    random_cpt(rng, prows, cards[i], zero),
                ],
            )
            continue
        # A parameterised factor: grounding turns it into one table per
        # instance over its missing parents, which check_model reads back.
        if kind == "noisy_or":
            params = [rng.random() for _ in range(1 + len(parents))]
        else:
            width = 1 + sum(cards[p] for p in parents)
            params = [rng.uniform(-3, 3) for _ in range((cards[i] - 1) * width)]
        cur.execute(
            "SELECT gaps.add_factor(%s, %s, %s, %s, kind => %s, params => %s)",
            [name, f"f{i}", f"s{i}", [f"s{p}" for p in parents], kind, params],
        )


def check_model(cur, name, seed):
    cur.execute("SELECT * FROM gaps._ground(gaps._model_id(%s))", [name])
    (
        card,
        evidence,
        child,
        parent_off,
        parents,
        cpt_off,
        cpts,
        var_attr,
    ) = cur.fetchone()
    n = len(card)
    # Entity names of this grounding's missing variables (0-based here).
    cur.execute("SELECT v, entity FROM gaps._missing_names(gaps._model_id(%s))", [name])
    var_entity = {v - 1: entity for v, entity in cur.fetchall()}

    bn = DiscreteBayesianNetwork()
    bn.add_nodes_from(range(n))
    for j in range(n):
        ps = parents[parent_off[j] - 1 : parent_off[j + 1] - 1]
        for p in ps:
            bn.add_edge(p - 1, child[j] - 1)
    for j in range(n):
        c = child[j] - 1
        ps = [p - 1 for p in parents[parent_off[j] - 1 : parent_off[j + 1] - 1]]
        k = card[c]
        nrows = 1
        for p in ps:
            nrows *= card[p]
        flat = cpts[cpt_off[j] - 1 : cpt_off[j] - 1 + nrows * k]
        # pgmpy's table is child-by-parent-combination, parents in order with
        # the last varying fastest: transpose our row-major layout.
        table = [[flat[r * k + x] for r in range(nrows)] for x in range(k)]
        bn.add_cpds(
            TabularCPD(
                c,
                k,
                table,
                evidence=ps or None,
                evidence_card=[card[p] for p in ps] or None,
            )
        )

    ev = {v: evidence[v] - 1 for v in range(n) if evidence[v] > 0}
    cur.execute(
        "SELECT entity, attname, value, probability FROM gaps.marginals(%s)", [name]
    )
    ours = {}
    for entity, att, value, p in cur.fetchall():
        ours[(json.dumps(entity, sort_keys=True), att, value)] = p

    attnames = {}
    cur.execute(
        "SELECT attribute_id, attname, domain FROM gaps.attribute a "
        "JOIN gaps.model m USING (model_id) WHERE m.name = %s",
        [name],
    )
    for aid, att, dom in cur.fetchall():
        attnames[aid] = (att, dom)

    infer = VariableElimination(bn)
    checked = 0
    for v in range(n):
        if evidence[v] > 0 or var_attr[v] is None:
            continue
        att, dom = attnames[var_attr[v]]
        q = infer.query([v], evidence=ev, show_progress=False)
        for x in range(card[v]):
            want = float(q.values[x])
            key = (json.dumps(var_entity[v], sort_keys=True), att, dom[x])
            got = ours.get(key)
            if got is None or abs(got - want) > TOL:
                print(
                    f"FAIL (model seed {seed}): {key}: pg_gaps {got}, pgmpy {want}",
                    file=sys.stderr,
                )
                sys.exit(1)
            checked += 1
    if checked == 0 and any(e == 0 for e in evidence):
        print(f"FAIL (model seed {seed}): nothing compared", file=sys.stderr)
        sys.exit(1)
    return checked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--models", type=int, default=200)
    args = ap.parse_args()
    warnings.filterwarnings("ignore")

    total = models = skipped = 0
    with psycopg.connect(autocommit=False) as conn:
        cur = conn.cursor()
        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_gaps")
        conn.commit()
        for i in range(args.models):
            seed = args.seed * 100003 + i
            rng = random.Random(seed)
            name = "pgmpy_m"
            try:
                build_model(cur, rng, name)
                total += check_model(cur, name, seed)
                models += 1
            except psycopg.errors.DataException as e:
                # Evidence with probability zero: pgmpy cannot condition on it
                # either.  Count it, and require that it is the reason.
                if "probability zero" not in str(e):
                    raise
                skipped += 1
            conn.rollback()
    if models == 0 or total == 0:
        print("FAIL: no model was compared", file=sys.stderr)
        sys.exit(1)
    print(
        f"pgmpy: {total} marginals agree to {TOL} over {models} models "
        f"({skipped} skipped: impossible evidence)"
    )


if __name__ == "__main__":
    main()
