/*-------------------------------------------------------------------------
 *
 * queryid_mask.h
 *		Collapse temporary relation identities in the query fingerprint.
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 *
 *-------------------------------------------------------------------------
 */
#ifndef QUERYID_MASK_H
#define QUERYID_MASK_H

/* GUC: is the query id recomputed with temporary relations collapsed? */
extern bool queryid_mask_temp_names;

/*
 * Define the GUC and install the hook.  Call from _PG_init(), before
 * MarkGUCPrefixReserved().
 */
extern void queryid_mask_init(void);

#endif							/* QUERYID_MASK_H */
