#include "builtin.h"
#include "natalie.h"

NatObject *Exception_new(NatEnv *env, NatObject *self, size_t argc, NatObject **args, struct hashmap *kwargs, NatBlock *block) {
    NatObject *exception = Object_new(env, self, argc, args, kwargs, block);
    exception->type = NAT_VALUE_EXCEPTION;
    if (exception->message == NULL) exception->message = heap_string(self->class_name);
    return exception;
}

NatObject *Exception_initialize(NatEnv *env, NatObject *self, size_t argc, NatObject **args, struct hashmap *kwargs, NatBlock *block) {
    if (argc == 0) {
        self->message = heap_string(self->class_name);
    } else if (argc == 1) {
        NatObject *message = args[0];
        if (NAT_TYPE(message) != NAT_VALUE_STRING) {
            message = nat_send(env, message, "inspect", 0, NULL, NULL);
        }
        self->message = heap_string(message->str);
    }
    return self;
}

NatObject *Exception_inspect(NatEnv *env, NatObject *self, size_t argc, NatObject **args, struct hashmap *kwargs, NatBlock *block) {
    NAT_ASSERT_ARGC(0);
    assert(NAT_TYPE(self) == NAT_VALUE_EXCEPTION);
    NatObject *str = nat_string(env, "#<");
    assert(NAT_OBJ_CLASS(self));
    nat_string_append(env, str, Module_inspect(env, NAT_OBJ_CLASS(self), 0, NULL, NULL, NULL)->str);
    nat_string_append(env, str, ": ");
    nat_string_append(env, str, self->message);
    nat_string_append_char(env, str, '>');
    return str;
}

NatObject *Exception_message(NatEnv *env, NatObject *self, size_t argc, NatObject **args, struct hashmap *kwargs, NatBlock *block) {
    NAT_ASSERT_ARGC(0);
    assert(NAT_TYPE(self) == NAT_VALUE_EXCEPTION);
    return nat_string(env, self->message);
}

NatObject *Exception_backtrace(NatEnv *env, NatObject *self, size_t argc, NatObject **args, struct hashmap *kwargs, NatBlock *block) {
    NAT_ASSERT_ARGC(0);
    assert(NAT_TYPE(self) == NAT_VALUE_EXCEPTION);
    return self->backtrace;
}
