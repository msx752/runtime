/*
 * generic-sharing.c: Support functions for generic sharing.
 *
 * Author:
 *   Mark Probst (mark.probst@gmail.com)
 *
 * (C) 2007 Novell, Inc.
 */

#include <config.h>

#include <mono/metadata/class.h>
#include <mono/utils/mono-counters.h>

#include "mini.h"

/*
 * mini_method_get_context:
 * @method: a method
 *
 * Returns the generic context of a method or NULL if it doesn't have
 * one.  For an inflated method that's the context stored in the
 * method.  Otherwise it's in the method's generic container or in the
 * generic container of the method's class.
 */
MonoGenericContext*
mini_method_get_context (MonoMethod *method)
{
	if (method->is_inflated)
		return mono_method_get_context (method);
	if (method->is_generic)
		return &(mono_method_get_generic_container (method)->context);
	if (method->klass->generic_container)
		return &method->klass->generic_container->context;
	return NULL;
}

/*
 * mono_method_check_context_used:
 * @method: a method
 *
 * Checks whether the method's generic context uses a type variable.
 * Returns an int with the bits MONO_GENERIC_CONTEXT_USED_CLASS and
 * MONO_GENERIC_CONTEXT_USED_METHOD set to reflect whether the
 * context's class or method instantiation uses type variables.
 */
int
mono_method_check_context_used (MonoMethod *method)
{
	MonoGenericContext *method_context = mini_method_get_context (method);

	if (!method_context)
		return 0;

	return mono_generic_context_check_used (method_context);
}

static gboolean
generic_inst_is_sharable (MonoGenericInst *inst, gboolean allow_type_vars)
{
	int i;

	for (i = 0; i < inst->type_argc; ++i) {
		MonoType *type = inst->type_argv [i];
		int type_type;

		if (MONO_TYPE_IS_REFERENCE (type))
			continue;

		type_type = mono_type_get_type (type);
		if (allow_type_vars && (type_type == MONO_TYPE_VAR || type_type == MONO_TYPE_MVAR))
			continue;

		return FALSE;
	}

	return TRUE;
}

/*
 * mono_generic_context_is_sharable:
 * @context: a generic context
 *
 * Returns whether the generic context is sharable.  A generic context
 * is sharable iff all of its type arguments are reference type.
 */
gboolean
mono_generic_context_is_sharable (MonoGenericContext *context, gboolean allow_type_vars)
{
	g_assert (context->class_inst || context->method_inst);

	if (context->class_inst && !generic_inst_is_sharable (context->class_inst, allow_type_vars))
		return FALSE;

	if (context->method_inst && !generic_inst_is_sharable (context->method_inst, allow_type_vars))
		return FALSE;

	return TRUE;
}

/*
 * mono_method_is_generic_impl:
 * @method: a method
 *
 * Returns whether the method is either inflated or part of an
 * inflated class.
 */
gboolean
mono_method_is_generic_impl (MonoMethod *method)
{
	return method->klass->generic_class != NULL && method->is_inflated;
}

/*
 * mono_method_is_generic_sharable_impl:
 * @method: a method
 *
 * Returns TRUE iff the method is inflated or part of an inflated
 * class, its context is sharable and it has no constraints on its
 * type parameters.  Otherwise returns FALSE.
 */
gboolean
mono_method_is_generic_sharable_impl (MonoMethod *method)
{
	if (!mono_method_is_generic_impl (method))
		return FALSE;

	if (method->is_inflated) {
		MonoMethodInflated *inflated = (MonoMethodInflated*)method;
		MonoGenericContext *context = &inflated->context;

		if (!mono_generic_context_is_sharable (context, FALSE))
			return FALSE;

		g_assert (inflated->declaring);

		if (inflated->declaring->is_generic) {
			g_assert (mono_method_get_generic_container (inflated->declaring)->type_params);

			if (mono_method_get_generic_container (inflated->declaring)->type_params->constraints)
				return FALSE;
		}
	}

	if (method->klass->generic_class) {
		if (!mono_generic_context_is_sharable (&method->klass->generic_class->context, FALSE))
			return FALSE;

		g_assert (method->klass->generic_class->container_class &&
				method->klass->generic_class->container_class->generic_container &&
				method->klass->generic_class->container_class->generic_container->type_params);

		if (method->klass->generic_class->container_class->generic_container->type_params->constraints)
			return FALSE;
	}

	return TRUE;
}

static gboolean
generic_inst_equal (MonoGenericInst *inst1, MonoGenericInst *inst2)
{
	int i;

	if (!inst1) {
		g_assert (!inst2);
		return TRUE;
	}

	g_assert (inst2);

	if (inst1->type_argc != inst2->type_argc)
		return FALSE;

	for (i = 0; i < inst1->type_argc; ++i)
		if (!mono_metadata_type_equal (inst1->type_argv [i], inst2->type_argv [i]))
			return FALSE;

	return TRUE;
}

/*
 * mono_generic_context_equal_deep:
 * @context1: a generic context
 * @context2: a generic context
 *
 * Returns whether context1's type arguments are equal to context2's
 * type arguments.
 */
gboolean
mono_generic_context_equal_deep (MonoGenericContext *context1, MonoGenericContext *context2)
{
	return generic_inst_equal (context1->class_inst, context2->class_inst) &&
		generic_inst_equal (context1->method_inst, context2->method_inst);
}

/*
 * mini_class_get_container_class:
 * @class: a generic class
 *
 * Returns the class's container class, which is the class itself if
 * it doesn't have generic_class set.
 */
MonoClass*
mini_class_get_container_class (MonoClass *class)
{
	if (class->generic_class)
		return class->generic_class->container_class;

	g_assert (class->generic_container);
	return class;
}

/*
 * mini_class_get_context:
 * @class: a generic class
 *
 * Returns the class's generic context.
 */
MonoGenericContext*
mini_class_get_context (MonoClass *class)
{
	if (class->generic_class)
		return &class->generic_class->context;

	g_assert (class->generic_container);
	return &class->generic_container->context;
}

/*
 * mini_get_basic_type_from_generic:
 * @gsctx: a generic sharing context
 * @type: a type
 *
 * Returns a closed type corresponding to the possibly open type
 * passed to it.
 */
MonoType*
mini_get_basic_type_from_generic (MonoGenericSharingContext *gsctx, MonoType *type)
{
	if (!type->byref && (type->type == MONO_TYPE_VAR || type->type == MONO_TYPE_MVAR))
		g_assert (gsctx);

	return mono_type_get_basic_type_from_generic (type);
}

/*
 * mini_type_stack_size:
 * @gsctx: a generic sharing context
 * @t: a type
 * @align: Pointer to an int for returning the alignment
 *
 * Returns the type's stack size and the alignment in *align.  The
 * type is allowed to be open.
 */
int
mini_type_stack_size (MonoGenericSharingContext *gsctx, MonoType *t, int *align)
{
	return mono_type_stack_size_internal (t, align, gsctx != NULL);
}

/*
 * mono_generic_sharing_init:
 *
 * Register the generic sharing counters.
 */
void
mono_generic_sharing_init (void)
{
}
