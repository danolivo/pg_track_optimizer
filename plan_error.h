/*-------------------------------------------------------------------------
 *
 * plan_error.h
 *		Pass through a query plan and calculate estimation error.
 *
 * Copyright (c) 2024-2025 Andrei Lepikhov
 *
 * This software may be modified and distributed under the terms
 * of the MIT licence. See the LICENSE file for details.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLAN_ERROR_H
#define PLAN_ERROR_H

#include "nodes/nodeFuncs.h"

/*
 * Data structure used for error estimation as well as for statistics gathering.
 */
typedef struct PlanEstimatorContext
{
	double	totaltime;

	/* Number of nodes assessed */
	int		nnodes;

	/*
	 * Total number of nodes in the plan. Originally, used to detect leaf nodes.
	 * Now, it is a part of statistics.
	 */
	int 	counter;

	/* Different types of planning error may be placed here */
	double	mean_error;
	double	rms_error;
	double	time_error;
} PlanEstimatorContext;

#endif /* PLAN_ERROR_H */

double plan_error(PlanState *pstate, double totaltime, PlanEstimatorContext *ctx);
