#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <cloog/isl/cloog.h>
#include <cloog/isl/backend.h>
#include <isl_set.h>


#define ALLOC(type) (type*)malloc(sizeof(type))
#define ALLOCN(type,n) (type*)malloc((n)*sizeof(type))


/******************************************************************************
 *                             Memory leaks hunting                           *
 ******************************************************************************/


/**
 * These global variables are devoted to memory leaks hunting in the PolyLib
 * backend.  The isl backend has its own memory leak detection facilities.
 */
int cloog_matrix_allocated = 0;
int cloog_matrix_freed = 0;
int cloog_matrix_max = 0;


void cloog_constraint_set_free(CloogConstraintSet *constraints)
{
	isl_basic_set_free(constraints);
}


int cloog_constraint_set_contains_level(CloogConstraintSet *constraints,
			int level, int nb_parameters)
{
	return constraints->dim >= level;
}

/* Check if the variable at position level is defined by an
 * equality.  If so, return the row number.  Otherwise, return -1.
 */
CloogConstraint cloog_constraint_set_defining_equality(
	CloogConstraintSet *bset, int level)
{
	struct isl_basic_set_constraint c;

	if (isl_basic_set_has_defining_equality(bset, level - 1, &c))
		return c;
	else
		return cloog_constraint_invalid();
}


/* Check if the variable (e) at position level is defined by a
 * pair of inequalities
 *		 <a, i> + -m e +  <b, p> + k1 >= 0
 *		<-a, i> +  m e + <-b, p> + k2 >= 0
 * with 0 <= k1 + k2 < m
 * If so return the row number of the upper bound and set *lower
 * to the row number of the lower bound.  If not, return -1.
 *
 * If the variable at position level occurs in any other constraint,
 * then we currently return -1.  The modulo guard that we would generate
 * would still be correct, but we would also need to generate
 * guards corresponding to the other constraints, and this has not
 * been implemented yet.
 */
CloogConstraint cloog_constraint_set_defining_inequalities(
	CloogConstraintSet *bset,
	int level, CloogConstraint *lower, int nb_par)
{
	struct isl_basic_set_constraint upper;
	struct isl_basic_set_constraint c;

	if (!isl_basic_set_has_defining_inequalities(bset, level - 1,
								lower, &upper))
		return cloog_constraint_invalid();
	for (c = isl_basic_set_first_constraint(bset);
	     isl_basic_set_constraint_is_valid(c);
	     c = isl_basic_set_constraint_next(c)) {
		if (isl_basic_set_constraint_is_equal(c, *lower))
			continue;
		if (isl_basic_set_constraint_is_equal(c, upper))
			continue;
		if (cloog_constraint_involves(c, level-1))
			return cloog_constraint_invalid();
	}
	return upper;
}

int cloog_constraint_set_total_dimension(CloogConstraintSet *constraints)
{
	assert(constraints->n_div == 0);
	return constraints->nparam + constraints->dim;
}

int cloog_constraint_set_n_iterators(CloogConstraintSet *constraints, int n_par)
{
	return cloog_constraint_set_total_dimension(constraints) - n_par;
}


/******************************************************************************
 *                        Equalities spreading functions                      *
 ******************************************************************************/


/* Equalities are stored inside a CloogMatrix data structure called "equal".
 * This matrix has (nb_scattering + nb_iterators + 1) rows (i.e. total
 * dimensions + 1, the "+ 1" is because a statement can be included inside an
 * external loop without iteration domain), and (nb_scattering + nb_iterators +
 * nb_parameters + 2) columns (all unknowns plus the scalar plus the equality
 * type). The ith row corresponds to the equality "= 0" for the ith dimension
 * iterator. The first column gives the equality type (0: no equality, then
 * EQTYPE_* -see pprint.h-). At each recursion of pprint, if an equality for
 * the current level is found, the corresponding row is updated. Then the
 * equality if it exists is used to simplify expressions (e.g. if we have 
 * "i+1" while we know that "i=2", we simplify it in "3"). At the end of
 * the pprint call, the corresponding row is reset to zero.
 */

CloogEqualities *cloog_equal_alloc(int n, int nb_levels, int nb_parameters)
{
	int i;
	CloogEqualities *equal = ALLOC(CloogEqualities);

	equal->total_dim = nb_levels - 1 + nb_parameters;
	equal->n = n;
	equal->constraints = ALLOCN(struct isl_basic_set *, n);
	equal->types = ALLOCN(int, n);
	for (i = 0; i < n; ++i) {
		equal->constraints[i] = NULL;
		equal->types[i] = EQTYPE_NONE;
	}
	return equal;
}

int cloog_equal_total_dimension(CloogEqualities *equal)
{
	return equal->total_dim;
}

void cloog_equal_free(CloogEqualities *equal)
{
	int i;

	for (i = 0; i < equal->n; ++i)
		isl_basic_set_free(equal->constraints[i]);
	free(equal->constraints);
	free(equal->types);
	free(equal);
}

int cloog_equal_count(CloogEqualities *equal)
{
	return equal->n;
}


/**
 * cloog_constraint_equal_type function :
 * This function returns the type of the equality in the constraint (line) of
 * (constraints) for the element (level). An equality is 'constant' iff all
 * other
 * factors are null except the constant one. It is a 'pure item' iff one and
 * only one factor is non null and is 1 or -1. Otherwise it is an 'affine
 * expression'.
 * For instance:
 *   i = -13 is constant, i = j, j = -M are pure items,
 *   j = 2*M, i = j+1 are affine expressions.
 * When the equality comes from a 'one time loop', (line) is ONE_TIME_LOOP.
 * This case require a specific treatment since we have to study all the
 * constraints.
 * - constraints is the matrix of constraints,
 * - level is the column number in equal of the element which is 'equal to',
 * - line is the line number in equal of the constraint we want to study;
 *   if it is -1, all lines must be studied.
 */
static int cloog_constraint_equal_type(CloogConstraint constraint, int level)
{ 
	int i;
	isl_int c;
	int type = EQTYPE_NONE;
    
	isl_int_init(c);
	isl_basic_set_constraint_get_constant(constraint, &c);
	if (!isl_int_is_zero(c))
		type = EQTYPE_CONSTANT;
	for (i = 0; i < isl_basic_set_constraint_nparam(constraint); ++i) {
		isl_basic_set_constraint_get_param(constraint, i, &c);
		if (isl_int_is_zero(c))
			continue;
		if ((!isl_int_is_one(c) && !isl_int_is_negone(c)) ||
		    type != EQTYPE_NONE) {
			type = EQTYPE_EXAFFINE;
			break;
		}
		type = EQTYPE_PUREITEM;
	}
	for (i = 0; i < isl_basic_set_constraint_dim(constraint); ++i) {
		if (i == level - 1)
			continue;
		isl_basic_set_constraint_get_dim(constraint, i, &c);
		if (isl_int_is_zero(c))
			continue;
		if ((!isl_int_is_one(c) && !isl_int_is_negone(c)) ||
		    type != EQTYPE_NONE) {
			type = EQTYPE_EXAFFINE;
			break;
		}
		type = EQTYPE_PUREITEM;
	}
	for (i = 0; i < isl_basic_set_constraint_n_div(constraint); ++i) {
		isl_basic_set_constraint_get_div(constraint, i, &c);
		if (isl_int_is_zero(c))
			continue;
		if ((!isl_int_is_one(c) && !isl_int_is_negone(c)) ||
		    type != EQTYPE_NONE) {
			type = EQTYPE_EXAFFINE;
			break;
		}
		type = EQTYPE_PUREITEM;
	}
	isl_int_clear(c);

	if (type == EQTYPE_NONE)
		type = EQTYPE_CONSTANT;

	return type;
}


int cloog_equal_type(CloogEqualities *equal, int level)
{
	return equal->types[level-1];
}


/**
 * cloog_equal_add function:
 * This function updates the row (level-1) of the equality matrix (equal) with
 * the row that corresponds to the row (line) of the matrix (matrix).
 * - equal is the matrix of equalities,
 * - matrix is the matrix of constraints,
 * - level is the column number in matrix of the element which is 'equal to',
 * - line is the line number in matrix of the constraint we want to study,
 * - the infos structure gives the user all options on code printing and more.
 **
 * line is set to and invalid constraint for equalities that CLooG itself has
 * discovered because the lower and upper bound of a loop happened to be equal.
 * This situation shouldn't happen in the isl port since isl should
 * have found the equality itself.
 */
void cloog_equal_add(CloogEqualities *equal, CloogConstraintSet *matrix,
			int level, CloogConstraint line, int nb_par)
{ 
	struct isl_basic_set *bset;
	assert(cloog_constraint_is_valid(line));
  
	equal->types[level-1] = cloog_constraint_equal_type(line, level);
	bset = isl_basic_set_from_constraint(line);
	bset = isl_basic_set_extend(bset, bset->nparam,
				    equal->total_dim - bset->nparam, 0, 0, 0);
	equal->constraints[level-1] = bset;
}


/**
 * cloog_equal_del function :
 * This function reset the equality corresponding to the iterator (level)
 * in the equality matrix (equal).
 * - July 2nd 2002: first version. 
 */
void cloog_equal_del(CloogEqualities *equal, int level)
{ 
	equal->types[level-1] = EQTYPE_NONE;
	isl_basic_set_free(equal->constraints[level-1]);
	equal->constraints[level-1] = NULL;
}



/******************************************************************************
 *                            Processing functions                            *
 ******************************************************************************/

/**
 * Function cloog_constraint_set_normalize:
 * This function will modify the constraint system in such a way that when
 * there is an equality depending on the element at level 'level', there are
 * no more (in)equalities depending on this element.
 *
 * The simplified form of isl automatically satisfies this condition.
 */
void cloog_constraint_set_normalize(CloogConstraintSet *matrix, int level)
{
}



/**
 * cloog_constraint_set_copy function:
 * this functions builds and returns a "hard copy" (not a pointer copy) of a
 * CloogConstraintSet data structure.
 */
CloogConstraintSet *cloog_constraint_set_copy(CloogConstraintSet *bset)
{
	return isl_basic_set_dup(bset);
}


/**
 * cloog_constraint_set_simplify function:
 * this function simplify all constraints inside the matrix "matrix" thanks to
 * an equality matrix "equal" that gives for some elements of the affine
 * constraint an equality with other elements, preferably constants.
 * For instance, if a row of the matrix contains i+j+3>=0 and the equality
 * matrix gives i=n and j=2, the constraint is simplified to n+3>=0. The
 * simplified constraints are returned back inside a new simplified matrix.
 * - matrix is the set of constraints to simplify,
 * - equal is the matrix of equalities,
 * - level is a level we don't want to simplify (-1 if none),
 * - nb_par is the number of parameters of the program.
 **
 * isl should have performed these simplifications already in isl_set_gist.
 */
CloogConstraintSet *cloog_constraint_set_simplify(CloogConstraintSet *matrix,
	CloogEqualities *equal, int level, int nb_par)
{
	return cloog_constraint_set_copy(matrix);
}


/**
 * Return clast_expr corresponding to the variable "level" (1 based) in
 * the given constraint.
 */
struct clast_expr *cloog_constraint_variable_expr(CloogConstraint constraint,
	int level, CloogNames *names)
{
	int total_dim, nb_iter;
	const char *name;

	total_dim = cloog_constraint_total_dimension(constraint);
	nb_iter = total_dim - names->nb_parameters;

	if (level <= nb_iter)
		name = cloog_names_name_at_level(names, level);
	else
		name = names->parameters[level - (nb_iter+1)] ;

	return &new_clast_name(name)->expr;
}


/**
 * Return true if constraint c involves variable v (zero-based).
 */
int cloog_constraint_involves(CloogConstraint constraint, int v)
{
	isl_int c;
	int res;

	isl_int_init(c);
	cloog_constraint_coefficient_get(constraint, v, &c);
	res = !isl_int_is_zero(c);
	isl_int_clear(c);
	return res;
}

int cloog_constraint_is_lower_bound(CloogConstraint constraint, int v)
{
	isl_int c;
	int res;

	isl_int_init(c);
	cloog_constraint_coefficient_get(constraint, v, &c);
	res = isl_int_is_pos(c);
	isl_int_clear(c);
	return res;
}

int cloog_constraint_is_upper_bound(CloogConstraint constraint, int v)
{
	isl_int c;
	int res;

	isl_int_init(c);
	cloog_constraint_coefficient_get(constraint, v, &c);
	res = isl_int_is_neg(c);
	isl_int_clear(c);
	return res;
}

int cloog_constraint_is_equality(CloogConstraint constraint)
{
	return isl_basic_set_constraint_is_equality(constraint);
}

void cloog_constraint_clear(CloogConstraint constraint)
{
	isl_basic_set_constraint_clear(constraint);
}

void cloog_constraint_coefficient_get(CloogConstraint constraint,
			int var, cloog_int_t *val)
{
	struct isl_basic_set *bset = isl_basic_set_constraint_set(constraint);

	if (var < bset->dim)
		isl_basic_set_constraint_get_dim(constraint, var, val);
	else
		isl_basic_set_constraint_get_param(constraint,
							var - bset->dim, val);
}

void cloog_constraint_coefficient_set(CloogConstraint constraint,
			int var, cloog_int_t val)
{
	struct isl_basic_set *bset = isl_basic_set_constraint_set(constraint);

	if (var < bset->dim)
		isl_basic_set_constraint_set_dim(constraint, var, val);
	else
		isl_basic_set_constraint_set_param(constraint,
							var - bset->dim, val);
}

void cloog_constraint_constant_get(CloogConstraint constraint, cloog_int_t *val)
{
	isl_basic_set_constraint_get_constant(constraint, val);
}

/**
 * Copy the coefficient of constraint c into dst in PolyLib order,
 * i.e., first the coefficients of the variables, then the coefficients
 * of the parameters and finally the constant.
 */
void cloog_constraint_copy_coefficients(CloogConstraint constraint, cloog_int_t *dst)
{
	int i;
	unsigned dim;

	dim = cloog_constraint_set_total_dimension(
					isl_basic_set_constraint_set(constraint));

	for (i = 0; i < dim; ++i)
		cloog_constraint_coefficient_get(constraint, i, dst+i);
	cloog_constraint_constant_get(constraint, dst+dim);
}

CloogConstraint cloog_constraint_invalid(void)
{
	return isl_basic_set_constraint_invalid();
}

int cloog_constraint_is_valid(CloogConstraint constraint)
{
	return isl_basic_set_constraint_is_valid(constraint);
}

int cloog_constraint_total_dimension(CloogConstraint constraint)
{
	return cloog_constraint_set_total_dimension(
				isl_basic_set_constraint_set(constraint));
}

CloogConstraint cloog_constraint_first(CloogConstraintSet *constraints)
{
	return isl_basic_set_first_constraint(constraints);
}

CloogConstraint cloog_constraint_next(CloogConstraint constraint)
{
	return isl_basic_set_constraint_next(constraint);
}

void cloog_constraint_release(CloogConstraint constraint)
{
}

CloogConstraint cloog_constraint_copy(CloogConstraint constraint)
{
	return constraint;
}

CloogConstraint cloog_equal_constraint(CloogEqualities *equal, int j)
{
	return isl_basic_set_first_constraint(equal->constraints[j]);
}
