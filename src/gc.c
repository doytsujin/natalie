#include "gc.h"
#include "natalie.h"

void nat_gc_init(NatEnv *env, void *bottom_of_stack) {
    env->global_env->bottom_of_stack = bottom_of_stack;
    env->global_env->main_thread = pthread_self();
}

NatHeapBlock *nat_gc_alloc_heap_block(NatGlobalEnv *global_env) {
    NatHeapBlock *block = calloc(1, sizeof(NatHeapBlock));
    block->next = NULL;
    NatObject *last = block->free_list = &block->storage[0];
    for (size_t i = 1; i < NAT_HEAP_BLOCK_CELL_COUNT; i++) {
        NatObject *cell = &block->storage[i];
        last->next_free_object = cell;
        last = cell;
    }
    last->next_free_object = NULL;
    NatHeapBlock *next_block = global_env->heap;
    global_env->heap = block;
    block->next = next_block;
    if (&block->storage[NAT_HEAP_BLOCK_CELL_COUNT - 1] > global_env->max_ptr) {
        global_env->max_ptr = &block->storage[NAT_HEAP_BLOCK_CELL_COUNT - 1];
    }
    if (&block->storage[0] < global_env->min_ptr) {
        global_env->min_ptr = &block->storage[0];
    }
    global_env->cells_available += NAT_HEAP_BLOCK_CELL_COUNT;
    global_env->cells_total += NAT_HEAP_BLOCK_CELL_COUNT;
    return block;
}

NatObject *nat_gc_malloc(NatEnv *env) {
    NAT_LOCK_ALLOC(env);
    NatHeapBlock *block = env->global_env->heap;
    NatObject *cell;
    do {
        cell = block->free_list;
        if (cell) {
            block->free_list = cell->next_free_object;
            env->global_env->cells_available--;
            NAT_UNLOCK_ALLOC(env);
            return cell;
        }
        block = block->next;
    } while (block);
    nat_gc_alloc_heap_block(env->global_env);
    NAT_UNLOCK_ALLOC(env);
    return nat_gc_malloc(env);
}

static void nat_gc_push_object(NatEnv *env, NatObject *objects, NatObject *obj) {
    if (obj && !NAT_IS_TAGGED_INT(obj)) {

        // TODO: remove these checks later :-)
        if (obj < env->global_env->min_ptr || obj > env->global_env->max_ptr) {
            printf("GC fatal: memory corruption (obj %p outside of heap)\n", obj);
            abort();
        }
        if (obj->klass < env->global_env->min_ptr || obj->klass > env->global_env->max_ptr) {
            printf("GC fatal: memory corruption (obj %p klass outside of heap)\n", obj);
            abort();
        }

        nat_array_push(env, objects, obj);
    }
}

static void nat_gc_gather_from_env(NatObject *objects, NatEnv *env) {
    for (size_t i = 0; i < env->var_count; i++) {
        nat_gc_push_object(env, objects, env->vars[i]);
    }
    if (env->exception) nat_gc_push_object(env, objects, env->exception);
    if (env->outer) {
        nat_gc_gather_from_env(objects, env->outer);
    }
}

bool nat_gc_is_heap_ptr(NatEnv *env, NatObject *ptr) {
    NatHeapBlock *block = env->global_env->heap;
    do {
        if (ptr >= &block->storage[0] && ptr <= &block->storage[NAT_HEAP_BLOCK_CELL_COUNT - 1]) {
            for (size_t i = 0; i < NAT_HEAP_BLOCK_CELL_COUNT; i++) {
                if (ptr == &block->storage[i]) {
                    return true;
                }
            }
        }
        block = block->next;
    } while (block);
    return false;
}

NatObject *nat_gc_gather_roots(NatEnv *env) {
    NatGlobalEnv *global_env = env->global_env;
    void *dummy;
    void *top_of_stack = &dummy;
    NatObject *roots = nat_array(env);
    if (global_env->bottom_of_stack < top_of_stack) {
        fprintf(stderr, "Unsupported platform\n");
        abort();
    }
    for (void *p = global_env->bottom_of_stack; p >= top_of_stack; p -= 4) {
        NatObject *ptr = *((NatObject **)p);
        if (nat_gc_is_heap_ptr(env, ptr)) {
            nat_gc_push_object(env, roots, ptr);
        }
    }
    nat_gc_push_object(env, roots, global_env->Object);
    nat_gc_push_object(env, roots, global_env->Integer);
    nat_gc_push_object(env, roots, global_env->nil);
    nat_gc_push_object(env, roots, global_env->true_obj);
    nat_gc_push_object(env, roots, global_env->false_obj);
    struct hashmap_iter *iter;
    for (iter = hashmap_iter(global_env->globals); iter; iter = hashmap_iter_next(global_env->globals, iter)) {
        NatObject *obj = (NatObject *)hashmap_iter_get_data(iter);
        nat_gc_push_object(env, roots, obj);
    }
    nat_gc_gather_from_env(roots, env);
    return roots;
}

void nat_gc_unmark_all_objects(NatEnv *env) {
    NatHeapBlock *block = env->global_env->heap;
    do {
        for (size_t i = 0; i < NAT_HEAP_BLOCK_CELL_COUNT; i++) {
            block->storage[i].marked = false;
        }
        block = block->next;
    } while (block);
}

NatObject *nat_gc_mark_live_objects(NatEnv *env) {
    NatObject *objects = nat_gc_gather_roots(env);

    for (size_t o = 0; o < objects->ary_len; o++) {
        NatObject *obj = objects->ary[o];

        if (obj == objects) continue;
        if (obj->marked) continue;
        obj->marked = true;

        nat_gc_push_object(env, objects, NAT_OBJ_CLASS(obj));
        nat_gc_push_object(env, objects, obj->owner);
        if (obj->singleton_class) nat_gc_push_object(env, objects, obj->singleton_class);
        struct hashmap_iter *iter;
        if (obj->constants.table) {
            for (iter = hashmap_iter(&obj->constants); iter; iter = hashmap_iter_next(&obj->constants, iter)) {
                NatObject *o = (NatObject *)hashmap_iter_get_data(iter);
                nat_gc_push_object(env, objects, o);
            }
        }
        if (obj->ivars.table) {
            for (iter = hashmap_iter(&obj->ivars); iter; iter = hashmap_iter_next(&obj->ivars, iter)) {
                NatObject *o = (NatObject *)hashmap_iter_get_data(iter);
                nat_gc_push_object(env, objects, o);
            }
        }
        if (obj->cvars.table) {
            for (iter = hashmap_iter(&obj->cvars); iter; iter = hashmap_iter_next(&obj->cvars, iter)) {
                NatObject *o = (NatObject *)hashmap_iter_get_data(iter);
                nat_gc_push_object(env, objects, o);
            }
        }

        if (NAT_OBJ_HAS_ENV(obj)) {
            nat_gc_gather_from_env(objects, &obj->env);
        }

        NatHashKey *key;
        switch (obj->type) {
        case NAT_VALUE_ARRAY:
            for (size_t i = 0; i < obj->ary_len; i++) {
                nat_gc_push_object(env, objects, obj->ary[i]);
            }
            break;
        case NAT_VALUE_CLASS:
            nat_gc_push_object(env, objects, obj->superclass);
            for (size_t i = 0; i < obj->included_modules_count; i++) {
                nat_gc_push_object(env, objects, obj->included_modules[i]);
            }
            if (obj->methods.table) {
                for (iter = hashmap_iter(&obj->methods); iter; iter = hashmap_iter_next(&obj->methods, iter)) {
                    NatMethod *method = (NatMethod *)hashmap_iter_get_data(iter);
                    if (NAT_OBJ_HAS_ENV(method)) nat_gc_gather_from_env(objects, &method->env);
                }
            }
            break;
        case NAT_VALUE_ENCODING:
            nat_gc_push_object(env, objects, obj->encoding_names);
            break;
        case NAT_VALUE_EXCEPTION:
            nat_gc_push_object(env, objects, obj->backtrace);
            break;
        case NAT_VALUE_FALSE:
            break;
        case NAT_VALUE_HASH:
            if (obj->key_list) {
                key = obj->key_list;
                do {
                    nat_gc_push_object(env, objects, key->key);
                    nat_gc_push_object(env, objects, key->val);
                    key = key->next;
                } while (key != obj->key_list);
            }
            nat_gc_push_object(env, objects, obj->hash_default_value);
            if (obj->hash_default_block && NAT_OBJ_HAS_ENV(obj->hash_default_block)) nat_gc_gather_from_env(objects, &obj->hash_default_block->env);
            break;
        case NAT_VALUE_INTEGER:
            break;
        case NAT_VALUE_IO:
            break;
        case NAT_VALUE_MATCHDATA:
            break;
        case NAT_VALUE_MODULE:
            if (obj->methods.table) {
                for (iter = hashmap_iter(&obj->methods); iter; iter = hashmap_iter_next(&obj->methods, iter)) {
                    NatMethod *method = (NatMethod *)hashmap_iter_get_data(iter);
                    if (NAT_OBJ_HAS_ENV(method)) nat_gc_gather_from_env(objects, &method->env);
                }
            }
            break;
        case NAT_VALUE_NIL:
            break;
        case NAT_VALUE_OTHER:
            break;
        case NAT_VALUE_PROC:
            nat_gc_gather_from_env(objects, &obj->block->env);
            break;
        case NAT_VALUE_RANGE:
            nat_gc_push_object(env, objects, obj->range_begin);
            nat_gc_push_object(env, objects, obj->range_end);
            break;
        case NAT_VALUE_REGEXP:
            break;
        case NAT_VALUE_STRING:
            break;
        case NAT_VALUE_SYMBOL:
            break;
        case NAT_VALUE_THREAD:
            nat_gc_push_object(env, objects, obj->thread_value);
            break;
        case NAT_VALUE_TRUE:
            break;
        }
    }
    return objects;
}

static void nat_destroy_hash_key_list(NatObject *obj) {
    if (obj->key_list) {
        NatHashKey *key = obj->key_list;
        do {
            NatHashKey *next_key = key->next;
            free(key);
            key = next_key;
        } while (key != obj->key_list);
    }
}

static void nat_gc_collect_object(NatEnv *env, NatHeapBlock *block, NatObject *obj) {
    if (obj->constants.table) hashmap_destroy(&obj->constants);
    if (obj->ivars.table) hashmap_destroy(&obj->ivars);
    pthread_mutex_destroy(&obj->mutex);
    struct hashmap_iter *iter;
    switch (obj->type) {
    case NAT_VALUE_ARRAY:
        free(obj->ary);
        break;
    case NAT_VALUE_CLASS:
        free(obj->class_name);
        if (obj->methods.table) {
            for (iter = hashmap_iter(&obj->methods); iter; iter = hashmap_iter_next(&obj->methods, iter)) {
                NatMethod *method = (NatMethod *)hashmap_iter_get_data(iter);
                free(method);
            }
            hashmap_destroy(&obj->methods);
        }
        if (obj->cvars.table) hashmap_destroy(&obj->cvars);
        free(obj->included_modules);
        break;
    case NAT_VALUE_ENCODING:
        break;
    case NAT_VALUE_EXCEPTION:
        free(obj->message);
        break;
    case NAT_VALUE_FALSE:
        break;
    case NAT_VALUE_HASH:
        nat_destroy_hash_key_list(obj);
        for (iter = hashmap_iter(&obj->hashmap); iter; iter = hashmap_iter_next(&obj->hashmap, iter)) {
            free((NatHashVal *)hashmap_iter_get_data(iter));
        }
        hashmap_destroy(&obj->hashmap);
        free(obj->hash_default_block);
        break;
    case NAT_VALUE_INTEGER:
        break;
    case NAT_VALUE_IO:
        break;
    case NAT_VALUE_MATCHDATA:
        onig_region_free(obj->matchdata_region, true);
        free(obj->matchdata_str);
        break;
    case NAT_VALUE_MODULE:
        free(obj->class_name);
        if (obj->methods.table) {
            for (iter = hashmap_iter(&obj->methods); iter; iter = hashmap_iter_next(&obj->methods, iter)) {
                NatMethod *method = (NatMethod *)hashmap_iter_get_data(iter);
                free(method);
            }
            hashmap_destroy(&obj->methods);
        }
        if (obj->cvars.table) hashmap_destroy(&obj->cvars);
        free(obj->included_modules);
        break;
    case NAT_VALUE_NIL:
        break;
    case NAT_VALUE_OTHER:
        break;
    case NAT_VALUE_PROC:
        free(obj->block);
        break;
    case NAT_VALUE_RANGE:
        break;
    case NAT_VALUE_REGEXP:
        onig_free(obj->regexp);
        free(obj->regexp_str);
        break;
    case NAT_VALUE_STRING:
        free(obj->str);
        break;
    case NAT_VALUE_SYMBOL:
        free(obj->symbol);
        break;
    case NAT_VALUE_THREAD:
        break;
    case NAT_VALUE_TRUE:
        break;
    }
    obj->type = NAT_VALUE_NIL;
    obj->klass = NAT_NIL->klass;
    NAT_LOCK_ALLOC(env);
    NatObject *next_object = block->free_list;
    block->free_list = obj;
    obj->next_free_object = next_object;
    env->global_env->cells_available++;
    NAT_UNLOCK_ALLOC(env);
}

static void nat_gc_collect_dead_objects(NatEnv *env) {
    NatHeapBlock *block = env->global_env->heap;
    while (block) {
        for (size_t i = 0; i < NAT_HEAP_BLOCK_CELL_COUNT; i++) {
            NatObject *obj = &block->storage[i];
            if (obj->type && !obj->marked && NAT_TYPE(obj) != NAT_VALUE_SYMBOL) {
                nat_gc_collect_object(env, block, obj);
            }
        }
        block = block->next;
    }
}

void nat_gc_collect(NatEnv *env) {
#ifdef NAT_GC_DISABLE
    return;
#endif
    if (pthread_self() != env->global_env->main_thread) return;
    if (!env->global_env->gc_enabled) return; // FIXME: use a mutex :-)
    env->global_env->gc_enabled = false;
    nat_gc_unmark_all_objects(env);
    nat_gc_mark_live_objects(env);
    nat_gc_collect_dead_objects(env);
    while (nat_gc_cells_available_ratio(env) < NAT_HEAP_MIN_AVAIL_AFTER_COLLECTION_RATIO) {
        // still not enough, even after a collection
        nat_gc_alloc_heap_block(env->global_env);
    }
    env->global_env->gc_enabled = true;
}

void nat_gc_collect_all(NatEnv *env) {
    nat_gc_unmark_all_objects(env);
    nat_gc_collect_dead_objects(env);
}

double nat_gc_cells_available_ratio(NatEnv *env) {
    return (double)env->global_env->cells_available / (double)env->global_env->cells_total;
}

NatObject *nat_alloc(NatEnv *env, NatObject *klass, enum NatValueType type) {
#ifdef NAT_GC_COLLECT_DEBUG
    nat_gc_collect(env);
#else
    if (nat_gc_cells_available_ratio(env) < NAT_HEAP_MIN_AVAIL_RATIO) {
        nat_gc_collect(env);
    }
#endif
    NatObject *obj = nat_gc_malloc(env);
    memset(obj, 0, sizeof(NatObject));
    obj->klass = klass;
    obj->type = type;
    int err = pthread_mutex_init(&obj->mutex, NULL);
    if (err) {
        fprintf(stderr, "Could not initialize mutex: %d\n", err);
        abort();
    }
    return obj;
}
