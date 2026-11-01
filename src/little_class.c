#include "little.h"
#include "little_buffer_internal.h"
#include "little_runtime_internal.h"

#include <ctype.h>
#include <string.h>
static lt_Table* _lt_instance_private_fields_for(lt_VM* vm, lt_Object* instance, lt_Object* klass, uint8_t create)
{
	lt_Value key = LT_VALUE_OBJECT(klass);
	lt_Value fields = lt_table_get_raw_internal(&instance->instance.private_fields, key);
	if (!LT_IS_TABLE(fields))
	{
		if (!create) return 0;
		fields = lt_make_table(vm);
		lt_table_set_raw_internal(vm, &instance->instance.private_fields, key, fields);
	}
	return &LT_GET_OBJECT(fields)->table;
}

static lt_Object* _lt_superclass_of(lt_Object* klass)
{
	return klass ? klass->class_def.superclass : 0;
}

lt_Value lt_find_class_constructor_internal(lt_Object* klass)
{
	for (lt_Object* current = klass; current; current = _lt_superclass_of(current))
		if (!LT_IS_NULL(current->class_def.constructor)) return current->class_def.constructor;
	return LT_VALUE_NULL;
}

void lt_class_set_member_internal(lt_VM* vm, lt_Value class_value, lt_Value key, lt_Value value, int16_t encoded)
{
	lt_Object* klass = LT_GET_OBJECT(class_value);
	lt_Object* member_obj = LT_IS_OBJECT(value) ? LT_GET_OBJECT(value) : 0;
	if (member_obj && member_obj->type == LT_OBJECT_CLOSURE) member_obj->closure.owner_class = klass;
	else if (member_obj && member_obj->type == LT_OBJECT_FN) member_obj->fn.owner_class = klass;

	lt_ClassMemberType type = (lt_ClassMemberType)(encoded & 0x0F);
	lt_Visibility visibility = (encoded & 0x10) ? LT_VIS_PRIVATE : LT_VIS_PUBLIC;
	uint8_t is_override = (encoded & 0x20) != 0;

	/* Label native members before storing, so they are qualified with the class
	   name rather than the bare member key. */
	lt_name_value_internal(vm, klass->class_def.name, key, value);

	if (type == LT_CLASS_CONSTRUCTOR)
	{
		klass->class_def.constructor = value;
		return;
	}

	if (visibility == LT_VIS_PUBLIC)
	{
		uint8_t found_any_in_super = 0;
		uint8_t found_same_in_super = 0;
		for (lt_Object* current = _lt_superclass_of(klass); current; current = _lt_superclass_of(current))
		{
			if (lt_table_index_raw_internal(vm, &current->class_def.public_fields, key, 0) ||
				lt_table_index_raw_internal(vm, &current->class_def.public_getters, key, 0) ||
				lt_table_index_raw_internal(vm, &current->class_def.public_setters, key, 0) ||
				lt_table_index_raw_internal(vm, &current->class_def.public_methods, key, 0))
				found_any_in_super = 1;

			lt_Table* same_table = type == LT_CLASS_FIELD ? &current->class_def.public_fields :
				type == LT_CLASS_GETTER ? &current->class_def.public_getters :
				type == LT_CLASS_SETTER ? &current->class_def.public_setters : &current->class_def.public_methods;
			if (lt_table_index_raw_internal(vm, same_table, key, 0))
			{
				found_same_in_super = 1;
			}
		}
		if (found_any_in_super && !is_override) lt_runtime_error(vm, "Class member conflicts with inherited member; use override!");
		if (!found_same_in_super && is_override) lt_runtime_error(vm, "Override member has no matching inherited member!");
	}
	else if (is_override) lt_runtime_error(vm, "Private members cannot be override!");

	lt_Table* table = 0;
	if (type == LT_CLASS_FIELD) table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_fields : &klass->class_def.public_fields;
	else if (type == LT_CLASS_GETTER) table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_getters : &klass->class_def.public_getters;
	else if (type == LT_CLASS_SETTER) table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_setters : &klass->class_def.public_setters;
	else table = visibility == LT_VIS_PRIVATE ? &klass->class_def.private_methods : &klass->class_def.public_methods;

	lt_table_set_raw_internal(vm, table, key, value);
}

static uint8_t _lt_native_class_name_valid(const char* name)
{
	if (!name || !(isalpha((unsigned char)name[0]) || name[0] == '_')) return 0;
	for (const unsigned char* c = (const unsigned char*)name + 1; *c; ++c)
		if (!(isalnum(*c) || *c == '_')) return 0;
	static const char* reserved[] = {
		"true", "false", "null", "fn", "async", "await", "class", "extends",
		"override", "super", "public", "private", "constructor", "get", "set",
		"break", "var", "global", "if", "else", "elseif", "for", "in", "while",
		"with", "import", "return", "type", "typeof", "and", "or", "not", "is", "isnt", 0
	};
	for (uint32_t i = 0; reserved[i]; ++i)
		if (strcmp(name, reserved[i]) == 0) return 0;
	return 1;
}

lt_Value lt_class_create(lt_VM* vm, const char* name)
{
	if (!_lt_native_class_name_valid(name)) lt_runtime_error(vm, "Native class name must be a Little identifier!");
	lt_Value klass = lt_make_class_internal(vm, lt_make_string(vm, name));
	LT_GET_OBJECT(klass)->class_def.is_native_class = 1;
	return klass;
}

static lt_Object* _lt_make_native_class_member(lt_VM* vm, lt_Value klass, lt_NativeFn fn)
{
	if (!LT_IS_CLASS(klass) || !LT_GET_OBJECT(klass)->class_def.is_native_class)
		lt_runtime_error(vm, "Expected native class!");
	if (!fn) lt_runtime_error(vm, "Native class member callback cannot be null!");
	lt_Value native = lt_make_native(vm, fn);
	lt_Object* closure = lt_allocate(vm, LT_OBJECT_CLOSURE);
	closure->closure.function = native;
	closure->closure.captures = lt_buffer_new(sizeof(lt_Value));
	closure->closure.owner_class = LT_GET_OBJECT(klass);
	return closure;
}

void lt_class_set_constructor(lt_VM* vm, lt_Value klass, lt_NativeFn fn)
{
	lt_Object* member = _lt_make_native_class_member(vm, klass, fn);
	lt_class_set_member_internal(vm, klass, lt_make_string(vm, "constructor"), LT_VALUE_OBJECT(member), LT_CLASS_CONSTRUCTOR);
}

static void _lt_native_class_add_member(lt_VM* vm, lt_Value klass_value, const char* name, lt_NativeFn fn,
	lt_ClassMemberType type, lt_Visibility visibility, uint8_t is_override)
{
	if (!LT_IS_CLASS(klass_value) || !LT_GET_OBJECT(klass_value)->class_def.is_native_class)
		lt_runtime_error(vm, "Expected native class!");
	if (!_lt_native_class_name_valid(name)) lt_runtime_error(vm, "Native class member name must be a Little identifier!");
	if (visibility != LT_VIS_PUBLIC && visibility != LT_VIS_PRIVATE)
		lt_runtime_error(vm, "Invalid native class member visibility!");
	if (is_override > 1) lt_runtime_error(vm, "Invalid native class override flag!");
	if (is_override && visibility == LT_VIS_PRIVATE)
		lt_runtime_error(vm, "Private members cannot be override!");
	lt_Value key = lt_make_string(vm, name);
	lt_Object* klass = LT_GET_OBJECT(klass_value);
	lt_Table* tables[] = {
		&klass->class_def.public_fields, &klass->class_def.private_fields,
		&klass->class_def.public_getters, &klass->class_def.private_getters,
		&klass->class_def.public_setters, &klass->class_def.private_setters,
		&klass->class_def.public_methods, &klass->class_def.private_methods
	};
	lt_ClassMemberType existing_types[] = {
		LT_CLASS_FIELD, LT_CLASS_FIELD, LT_CLASS_GETTER, LT_CLASS_GETTER,
		LT_CLASS_SETTER, LT_CLASS_SETTER, LT_CLASS_METHOD, LT_CLASS_METHOD
	};
	for (uint32_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
	{
		if (!lt_table_index_raw_internal(vm, tables[i], key, 0)) continue;
		if ((type == LT_CLASS_GETTER && existing_types[i] == LT_CLASS_SETTER) ||
			(type == LT_CLASS_SETTER && existing_types[i] == LT_CLASS_GETTER)) continue;
		lt_runtime_error(vm, "Class members cannot share a name except matching get/set accessors!");
	}

	lt_Object* member = _lt_make_native_class_member(vm, klass_value, fn);
	int16_t encoded = (int16_t)(type | (visibility == LT_VIS_PRIVATE ? 0x10 : 0) | (is_override ? 0x20 : 0));
	lt_class_set_member_internal(vm, klass_value, key, LT_VALUE_OBJECT(member), encoded);
}

void lt_class_add_method(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override)
{
	_lt_native_class_add_member(vm, klass, name, fn, LT_CLASS_METHOD, visibility, is_override);
}

void lt_class_add_getter(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override)
{
	_lt_native_class_add_member(vm, klass, name, fn, LT_CLASS_GETTER, visibility, is_override);
}

void lt_class_add_setter(lt_VM* vm, lt_Value klass, const char* name, lt_NativeFn fn, lt_Visibility visibility, uint8_t is_override)
{
	_lt_native_class_add_member(vm, klass, name, fn, LT_CLASS_SETTER, visibility, is_override);
}

void lt_class_set_native_data_destroy(lt_VM* vm, lt_Value klass, lt_NativeDataDestroyFn destroy)
{
	if (!LT_IS_CLASS(klass) || !LT_GET_OBJECT(klass)->class_def.is_native_class)
		lt_runtime_error(vm, "Expected native class!");
	LT_GET_OBJECT(klass)->class_def.native_data_destroy = destroy;
}

static lt_Object* _lt_validate_native_instance(lt_VM* vm, lt_Value instance_value, lt_Value native_class_value)
{
	if (!LT_IS_INSTANCE(instance_value)) lt_runtime_error(vm, "Expected class instance!");
	if (!LT_IS_CLASS(native_class_value) || !LT_GET_OBJECT(native_class_value)->class_def.is_native_class)
		lt_runtime_error(vm, "Expected native class!");
	lt_Object* expected = LT_GET_OBJECT(native_class_value);
	for (lt_Object* current = LT_GET_OBJECT(instance_value)->instance.klass; current; current = _lt_superclass_of(current))
		if (current == expected) return LT_GET_OBJECT(instance_value);
	lt_runtime_error(vm, "Instance is not derived from the requested native class!");
	return 0;
}

void lt_instance_set_native_data(lt_VM* vm, lt_Value instance_value, lt_Value native_class_value, void* data)
{
	lt_Object* instance = _lt_validate_native_instance(vm, instance_value, native_class_value);
	if (!data) lt_runtime_error(vm, "Native instance data cannot be null!");
	if (instance->instance.native_data) lt_runtime_error(vm, "Native instance data is already set!");
	if (!LT_GET_OBJECT(native_class_value)->class_def.native_data_destroy)
		lt_runtime_error(vm, "Native class has no instance data destroy callback!");
	instance->instance.native_data = data;
	instance->instance.native_data_destroy = LT_GET_OBJECT(native_class_value)->class_def.native_data_destroy;
}

void* lt_instance_get_native_data(lt_VM* vm, lt_Value instance_value, lt_Value native_class_value)
{
	lt_Object* instance = _lt_validate_native_instance(vm, instance_value, native_class_value);
	return instance->instance.native_data;
}

void lt_instance_dispose_native_data(lt_VM* vm, lt_Value instance_value)
{
	if (!LT_IS_INSTANCE(instance_value)) lt_runtime_error(vm, "Expected class instance!");
	lt_Object* instance = LT_GET_OBJECT(instance_value);
	void* data = instance->instance.native_data;
	lt_NativeDataDestroyFn destroy = instance->instance.native_data_destroy;
	instance->instance.native_data = 0;
	instance->instance.native_data_destroy = 0;
	if (data && destroy) destroy(data);
}

void lt_instance_clear_native_data(lt_VM* vm, lt_Value instance_value)
{
	if (!LT_IS_INSTANCE(instance_value)) lt_runtime_error(vm, "Expected class instance!");
	lt_Object* instance = LT_GET_OBJECT(instance_value);
	instance->instance.native_data = 0;
	instance->instance.native_data_destroy = 0;
}

lt_Value lt_make_class_internal(lt_VM* vm, lt_Value name)
{
	lt_Object* klass = lt_allocate(vm, LT_OBJECT_CLASS);
	klass->class_def.name = name;
	lt_table_init_internal(&klass->class_def.public_fields);
	lt_table_init_internal(&klass->class_def.private_fields);
	lt_table_init_internal(&klass->class_def.public_methods);
	lt_table_init_internal(&klass->class_def.private_methods);
	lt_table_init_internal(&klass->class_def.public_getters);
	lt_table_init_internal(&klass->class_def.private_getters);
	lt_table_init_internal(&klass->class_def.public_setters);
	lt_table_init_internal(&klass->class_def.private_setters);
	klass->class_def.constructor = LT_VALUE_NULL;
	return LT_VALUE_OBJECT(klass);
}

void lt_class_set_superclass_internal(lt_VM* vm, lt_Value class_value, lt_Value superclass_value)
{
	if (!LT_IS_CLASS(class_value) || !LT_IS_CLASS(superclass_value)) lt_runtime_error(vm, "Expected class superclass!");
	lt_Object* klass = LT_GET_OBJECT(class_value);
	lt_Object* superclass = LT_GET_OBJECT(superclass_value);
	for (lt_Object* current = superclass; current; current = _lt_superclass_of(current))
		if (current == klass) lt_runtime_error(vm, "Class inheritance cycle!");
	klass->class_def.superclass = superclass;
}

static lt_Value _lt_make_instance(lt_VM* vm, lt_Object* klass)
{
	lt_Object* instance = lt_allocate(vm, LT_OBJECT_INSTANCE);
	instance->instance.klass = klass;
	lt_table_init_internal(&instance->instance.public_fields);
	lt_table_init_internal(&instance->instance.private_fields);
	return LT_VALUE_OBJECT(instance);
}

static void _lt_run_field_initializers(lt_VM* vm, lt_Value instance, lt_Table* initializers, lt_Table* fields)
{
	for (uint8_t i = 0; i < 16; ++i)
	{
		lt_Buffer* bucket = initializers->buckets + i;
		for (uint32_t j = 0; j < bucket->length; ++j)
		{
			lt_TablePair* pair = lt_buffer_at(bucket, j);
			lt_push(vm, instance);
			uint16_t nret = lt_exec_internal(vm, pair->value, 1);
			lt_Value value = LT_VALUE_NULL;
			if (nret > 0)
			{
				value = lt_pop(vm);
				for (uint16_t r = 1; r < nret; ++r) lt_pop(vm);
			}
			lt_table_set_raw_internal(vm, fields, pair->key, value);
		}
	}
}

static void _lt_run_class_field_initializers(lt_VM* vm, lt_Value instance, lt_Object* klass)
{
	if (klass->class_def.superclass) _lt_run_class_field_initializers(vm, instance, klass->class_def.superclass);
	_lt_run_field_initializers(vm, instance, &klass->class_def.public_fields, &LT_GET_OBJECT(instance)->instance.public_fields);
	lt_Table* private_fields = _lt_instance_private_fields_for(vm, LT_GET_OBJECT(instance), klass, 1);
	_lt_run_field_initializers(vm, instance, &klass->class_def.private_fields, private_fields);
}

lt_Value lt_class_call_internal(lt_VM* vm, lt_Value class_value, uint8_t argc)
{
	lt_Object* klass = LT_GET_OBJECT(class_value);
	lt_Value instance = _lt_make_instance(vm, klass);
	uint32_t root_mark = vm->temporary_roots.length;
	lt_Object* instance_obj = LT_GET_OBJECT(instance);
	lt_buffer_push(vm, &vm->temporary_roots, &instance_obj);
	uint16_t base = vm->top - argc;

	_lt_run_class_field_initializers(vm, instance, klass);

	lt_Value constructor = lt_find_class_constructor_internal(klass);

	if (!LT_IS_NULL(constructor))
	{
		for (uint8_t i = 0; i < argc; ++i)
			vm->stack[vm->top - i] = vm->stack[vm->top - i - 1];
		vm->stack[base] = instance;
		vm->top++;
		if (vm->top > LT_STACK_SIZE) lt_runtime_error(vm, "VM stack overflow!");

		uint16_t nret = lt_exec_internal(vm, constructor, argc + 1);
		while (nret-- > 0) lt_pop(vm);
	}

	vm->top = base;
	vm->temporary_roots.length = root_mark;
	return instance;
}

lt_Value lt_instance_get_internal(lt_VM* vm, lt_Value instance_value, lt_Value key)
{
	lt_Object* instance = LT_GET_OBJECT(instance_value);
	lt_Object* klass = instance->instance.klass;
	lt_Value result = LT_VALUE_NULL;

	if (vm->current && vm->current->class_context)
	{
		lt_Object* access = vm->current->class_context;
		lt_TablePair* private_getter = lt_table_index_raw_internal(vm, &access->class_def.private_getters, key, 0);
		if (private_getter)
		{
			lt_push(vm, instance_value);
			uint16_t nret = lt_exec_internal(vm, private_getter->value, 1);
			if (nret > 0)
			{
				result = vm->stack[vm->top - nret];
				while (nret-- > 0) lt_pop(vm);
			}
			return result;
		}
	}

	for (lt_Object* current = klass; current; current = _lt_superclass_of(current))
	{
		lt_TablePair* public_getter = lt_table_index_raw_internal(vm, &current->class_def.public_getters, key, 0);
		if (public_getter)
		{
			lt_push(vm, instance_value);
			uint16_t nret = lt_exec_internal(vm, public_getter->value, 1);
			if (nret > 0)
			{
				result = vm->stack[vm->top - nret];
				while (nret-- > 0) lt_pop(vm);
			}
			return result;
		}
	}

	if (vm->current && vm->current->class_context)
	{
		lt_Table* private_fields = _lt_instance_private_fields_for(vm, instance, vm->current->class_context, 0);
		lt_TablePair* private_field = private_fields ? lt_table_index_raw_internal(vm, private_fields, key, 0) : 0;
		if (private_field) return private_field->value;
	}

	lt_TablePair* public_field = lt_table_index_raw_internal(vm, &instance->instance.public_fields, key, 0);
	if (public_field) return public_field->value;

	if (vm->current && vm->current->class_context)
	{
		lt_Object* access = vm->current->class_context;
		lt_TablePair* private_method = lt_table_index_raw_internal(vm, &access->class_def.private_methods, key, 0);
		if (private_method) return private_method->value;
	}

	for (lt_Object* current = klass; current; current = _lt_superclass_of(current))
	{
		lt_TablePair* public_method = lt_table_index_raw_internal(vm, &current->class_def.public_methods, key, 0);
		if (public_method) return public_method->value;
	}

	return LT_VALUE_NULL;
}

void lt_instance_set_internal(lt_VM* vm, lt_Value instance_value, lt_Value key, lt_Value value)
{
	lt_Object* instance = LT_GET_OBJECT(instance_value);
	lt_Object* klass = instance->instance.klass;
	if (vm->current && vm->current->class_context)
	{
		lt_Object* access = vm->current->class_context;
		lt_TablePair* private_setter = lt_table_index_raw_internal(vm, &access->class_def.private_setters, key, 0);
		if (private_setter)
		{
			lt_push(vm, instance_value);
			lt_push(vm, value);
			uint16_t nret = lt_exec_internal(vm, private_setter->value, 2);
			while (nret-- > 0) lt_pop(vm);
			return;
		}
	}

	for (lt_Object* current = klass; current; current = _lt_superclass_of(current))
	{
		lt_TablePair* public_setter = lt_table_index_raw_internal(vm, &current->class_def.public_setters, key, 0);
		if (public_setter)
		{
			lt_push(vm, instance_value);
			lt_push(vm, value);
			uint16_t nret = lt_exec_internal(vm, public_setter->value, 2);
			while (nret-- > 0) lt_pop(vm);
			return;
		}
	}

	if (vm->current && vm->current->class_context)
	{
		lt_Table* private_fields = _lt_instance_private_fields_for(vm, instance, vm->current->class_context, 0);
		if (private_fields && lt_table_index_raw_internal(vm, private_fields, key, 0))
		{
			lt_table_set_raw_internal(vm, private_fields, key, value);
			return;
		}
	}

	lt_table_set_raw_internal(vm, &instance->instance.public_fields, key, value);
}
