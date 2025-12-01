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
	double	error;
	double	time_weighted_error;
} PlanEstimatorContext;

#endif /* PLAN_ERROR_H */

double plan_error(PlanState *pstate, double totaltime, PlanEstimatorContext *ctx);
