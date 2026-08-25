/*-------------------------------------------------------------------------
 *
 * queryid_mask.c
 *		Mask temporary relation names in the query id.
 *
 * "Mask", not "squash": core already uses squashing for something else - a
 * long list of constants folded into one jumble element - and reusing the word
 * here would suggest the two are related.  This is narrower: one relation name
 * is replaced by a fixed one before the fingerprint is taken.
 *
 * Applications that generate SQL mechanically name their temporary tables
 * after a per-session counter, so one logical statement reaches the server as
 * "tt165" in one session and as "tt1551" in the next.  The core jumbler tells
 * those apart, which hands every execution its own entry: the shape of the
 * workload then disappears under thousands of single-call rows, and the hash
 * table fills with keys that will never be seen again.
 *
 * With pg_track_optimizer.queryid_mask_temp_names set, every temporary relation
 * contributes the same fixed identity to the fingerprint, so statements that
 * differ in nothing but the name of a temporary table share one query id and
 * accumulate into a single entry.  What still tells statements apart is the
 * rest of the parse tree: target list, varattno, join structure, quals.
 *
 * Note this deliberately merges statements that a user might consider
 * distinct - two unrelated temporary tables of the same shape queried the same
 * way become one entry - which is why the behaviour is off by default.
 *
 * Implementation note: the masking is done on the parse tree rather than
 * inside the jumbler, because the jumbler is not extensible.  Core resolves
 * pg_node_attr(custom_query_jumble) to a statically named function at compile
 * time, so an extension cannot contribute one.  What core does export is
 * JumbleQuery(), and that is enough: mask the relation identities, ask core to
 * recompute the fingerprint with its own generated walker, then restore the
 * tree before anything else looks at it.  The cost is one extra pass of the
 * jumbler per parse, paid only while the feature is on.
 *
 * Two fields are masked, not one, because which of them carries the relation's
 * identity depends on the server version.  In PostgreSQL 17 the fingerprint
 * uses RangeTblEntry.relid, while eref is query_jumble_ignore; since
 * PostgreSQL 18 it is the other way round - relid became query_jumble_ignore
 * and the name arrives through eref->aliasname (commit 30f8066989, "Add custom
 * query jumble function for RangeTblEntry.eref").  Masking only the name would
 * therefore be silently useless on 17.  Masking both keeps one code path
 * correct on either, and keeps working if the choice changes again.
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 *
 * IDENTIFICATION
 *	  contrib/pg_track_optimizer/queryid_mask.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_class.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "parser/analyze.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "queryid_mask.h"

/*
 * post_parse_analyze_hook receives the JumbleState by const pointer from
 * PostgreSQL 19 on.  We only pass it along, so a typedef is enough to keep one
 * definition of the hook compiling everywhere.
 */
#if PG_VERSION_NUM >= 190000
typedef const JumbleState *pgto_jumble_state;
#else
typedef JumbleState *pgto_jumble_state;
#endif

/*
 * Identity handed to the jumbler in place of a temporary relation's own.  The
 * leading byte cannot appear in a relation name, so no real table collides
 * with it, and InvalidOid is never the OID of a live relation.
 */
#define TEMP_RELATION_ALIAS		"\001temp"
#define TEMP_RELATION_OID		InvalidOid

bool		queryid_mask_temp_names = false;

static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;

/* One masked range table entry and the identity taken away from it. */
typedef struct MaskedRelation
{
	RangeTblEntry *rte;
	char	   *aliasname;
	Oid			relid;
} MaskedRelation;

typedef struct MaskContext
{
	List	   *masked;			/* MaskedRelation * */
} MaskContext;

static bool
mask_temp_relations_walker(Node *node, void *context)
{
	MaskContext *ctx = (MaskContext *) context;

	if (node == NULL)
		return false;

	if (IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		/*
		 * Only plain relations carry a name that a generator may have
		 * numbered; other RTE kinds have nothing to mask.
		 *
		 * Returning false does not skip the entry's contents.  With
		 * QTW_EXAMINE_RTES_BEFORE the walker shows us the RangeTblEntry and
		 * then descends into it itself, so a subquery or a function
		 * expression underneath is still visited - we only decline to walk
		 * the node a second time.
		 */
		if (rte->rtekind == RTE_RELATION &&
			OidIsValid(rte->relid) &&
			rte->eref != NULL &&
			rte->eref->aliasname != NULL &&
			get_rel_persistence(rte->relid) == RELPERSISTENCE_TEMP)
		{
			MaskedRelation *saved = palloc(sizeof(MaskedRelation));

			saved->rte = rte;
			saved->aliasname = rte->eref->aliasname;
			saved->relid = rte->relid;
			ctx->masked = lappend(ctx->masked, saved);

			rte->eref->aliasname = pstrdup(TEMP_RELATION_ALIAS);
			rte->relid = TEMP_RELATION_OID;
		}

		return false;
	}

	if (IsA(node, Query))
		return query_tree_walker((Query *) node, mask_temp_relations_walker,
								 context, QTW_EXAMINE_RTES_BEFORE);

	return expression_tree_walker(node, mask_temp_relations_walker, context);
}

/*
 * Put the real identities back.  The tree goes on to the rewriter and the
 * planner, so a mask left behind would not be a cosmetic problem: it would
 * hand them a relation with OID 0.
 *
 * Allocates nothing and cannot fail, which is what makes it usable from
 * PG_FINALLY.
 */
static void
unmask_temp_relations(MaskContext *ctx)
{
	ListCell   *lc;

	foreach(lc, ctx->masked)
	{
		MaskedRelation *saved = (MaskedRelation *) lfirst(lc);

		saved->rte->eref->aliasname = saved->aliasname;
		saved->rte->relid = saved->relid;
	}
}

/*
 * Recompute the query id with temporary relations collapsed.
 *
 * Runs before the previous hook so that pg_stat_statements and anything else
 * downstream observes the collapsed id.  For that to hold, this module has to
 * appear in shared_preload_libraries *after* those extensions: hooks are
 * called in the reverse of the order they were installed.
 */
static void
queryid_mask_post_parse_analyze(ParseState *pstate, Query *query,
						  pgto_jumble_state jstate)
{
	if (queryid_mask_temp_names && IsQueryIdEnabled() && query->queryId != 0)
	{
		MaskContext ctx = {NIL};

		(void) query_tree_walker(query, mask_temp_relations_walker, &ctx,
								 QTW_EXAMINE_RTES_BEFORE);

		if (ctx.masked != NIL)
		{
			/*
			 * JumbleQuery() writes the result into query->queryId itself.  The
			 * JumbleState it returns is discarded: no constant moved, so the
			 * locations recorded in the caller's jstate stay valid and remain
			 * the right basis for a normalized query text.
			 */
			PG_TRY();
			{
				(void) JumbleQuery(query);
			}
			PG_FINALLY();
			{
				unmask_temp_relations(&ctx);
			}
			PG_END_TRY();
		}
	}

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);
}

void
queryid_mask_init(void)
{
	DefineCustomBoolVariable("pg_track_optimizer.queryid_mask_temp_names",
							 "Give every temporary relation the same identity in the query id",
							 "Statements differing only in the name of a temporary "
							 "table then share one entry.  Entries recorded before "
							 "the setting changed keep their old key, so reset the "
							 "statistics after toggling it",
							 &queryid_mask_temp_names,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = queryid_mask_post_parse_analyze;
}
