/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2009 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Michael Grunder <michael.grunder@gmail.com>                  |
  | Maintainer: Nicolas Favre-Felix <n.favre-felix@owlient.eu>           |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "php_redis.h"
#include "ext/standard/info.h"
#include "crc16.h"
#include "redis_cluster.h"
#include "redis_commands.h"
#include <zend_exceptions.h>
#include "library.h"

zend_class_entry *redis_cluster_ce;

/* Exception handler */
zend_class_entry *redis_cluster_exception_ce;
zend_class_entry *spl_rte_ce = NULL;

/* Function table */
zend_function_entry redis_cluster_functions[] = {
    PHP_ME(RedisCluster, __construct, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, get, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, set, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, setex, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, psetex, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, setnx, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, getset, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, exists, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, keys, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, type, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, lpop, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, rpop, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, spop, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(RedisCluster, strlen, NULL, ZEND_ACC_PUBLIC)
    {NULL, NULL, NULL}
};

/* Our context seeds will be a hash table with RedisSock* pointers */
static void ht_free_seed(void *data) {
    RedisSock *redis_sock = *(RedisSock**)data;
    if(redis_sock) redis_free_socket(redis_sock);
}

/* Free redisClusterNode objects we've stored */
static void ht_free_node(void *data) {
    redisClusterNode *node = *(redisClusterNode**)data;
    cluster_free_node(node);
}

/* Initialize/Register our RedisCluster exceptions */
PHPAPI zend_class_entry *rediscluster_get_exception_base(int root TSRMLS_DC) {
#if HAVE_SPL
    if(!root) {
        if(!spl_rte_ce) {
            zend_class_entry **pce;

            if(zend_hash_find(CG(class_table), "runtimeexception",
                              sizeof("runtimeexception"), (void**)&pce)
                              ==SUCCESS)
            {
                spl_rte_ce = *pce;
                return *pce;
            }
        } else {
            return spl_rte_ce;
        }
    }
#endif
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 2)
    return zend_exception_get_default();
#else
    return zend_exception_get_default(TSRMLS_C);
#endif
}

/* Create redisCluster context */
zend_object_value
create_cluster_context(zend_class_entry *class_type TSRMLS_DC) {
    zend_object_value retval;
    redisCluster *cluster;

    // Allocate our actual struct
    cluster = emalloc(sizeof(redisCluster));
    memset(cluster, 0, sizeof(redisCluster));

    // Allocate our RedisSock we'll use to store prefix/serialization flags
    cluster->flags = ecalloc(1, sizeof(RedisSock));

    // Allocate our hash table for seeds
    ALLOC_HASHTABLE(cluster->seeds);
    zend_hash_init(cluster->seeds, 0, NULL, ht_free_seed, 0);

    // Allocate our hash table for connected Redis objects
    ALLOC_HASHTABLE(cluster->nodes);
    zend_hash_init(cluster->nodes, 0, NULL, ht_free_node, 0);

    // Initialize it
    zend_object_std_init(&cluster->std, class_type TSRMLS_CC);

#if PHP_VERSION_ID < 50399
    zval *tmp;

    zend_hash_copy(cluster->std.properties, &class_type->default_properties,
        (copy_ctor_func_t)zval_add_ref, (void*)&tmp, sizeof(zval*));
#endif

    retval.handle = zend_objects_store_put(cluster,
        (zend_objects_store_dtor_t)zend_objects_destroy_object,
        free_cluster_context, NULL TSRMLS_CC);

    retval.handlers = zend_get_std_object_handlers();

    return retval;
}

/* Free redisCluster context */
void free_cluster_context(void *object TSRMLS_DC) {
    redisCluster *cluster;

    // Grab context
    cluster = (redisCluster*)object;

    // Free any allocated prefix, as well as the struct
    if(cluster->flags->prefix) efree(cluster->flags->prefix);
    efree(cluster->flags);

    // Free seeds HashTable itself
    zend_hash_destroy(cluster->seeds);
    efree(cluster->seeds);

    // Destroy all Redis objects and free our nodes HashTable
    zend_hash_destroy(cluster->nodes);
    efree(cluster->nodes);

    // Finally, free the redisCluster structure itself
    efree(cluster);
}

//
// PHP Methods
//

/* Create a RedisCluster Object */
PHP_METHOD(RedisCluster, __construct) {
    zval *object, *z_seeds=NULL;
    char *name;
    long name_len;
    double timeout = 0.0, read_timeout = 0.0;
    redisCluster *context = GET_CONTEXT();

    // Parse arguments
    if(zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), 
                                    "Os|add", &object, redis_cluster_ce, &name, 
                                    &name_len, &z_seeds, &timeout, 
                                    &read_timeout)==FAILURE)
    {
        RETURN_FALSE;
    }
       
    // Require a name
    if(name_len == 0) {
        zend_throw_exception(redis_cluster_exception_ce,
            "You must give this cluster a name!",
            0 TSRMLS_CC);
    }

    // Validate timeout
    if(timeout < 0L || timeout > INT_MAX) {
        zend_throw_exception(redis_cluster_exception_ce,
            "Invalid timeout", 0 TSRMLS_CC);
        RETURN_FALSE;
    }
    
    // Validate our read timeout
    if(read_timeout < 0L || read_timeout > INT_MAX) {
        zend_throw_exception(redis_cluster_exception_ce,
            "Invalid read timeout", 0 TSRMLS_CC);
        RETURN_FALSE;
    }
    
    // TODO: Implement seed retrieval from php.ini
    if(!z_seeds || zend_hash_num_elements(Z_ARRVAL_P(z_seeds))==0) {
        zend_throw_exception(redis_cluster_exception_ce,
            "Must pass seeds", 0 TSRMLS_CC);
        RETURN_FALSE;
    }
    
    // Initialize our RedisSock "seed" objects
    cluster_init_seeds(context, Z_ARRVAL_P(z_seeds));
    
    // Create and map our key space
    cluster_map_keyspace(context TSRMLS_CC);
}

/*
 * RedisCluster methods
 */

/* {{{ proto string RedisCluster::get(string key) */
PHP_METHOD(RedisCluster, get) {
    CLUSTER_PROCESS_KW_CMD("GET", redis_gen_key_cmd, cluster_bulk_resp);
}
/* }}} */

/* {{{ proto bool RedisCluster::set(string key, string value) */
PHP_METHOD(RedisCluster, set) {
    CLUSTER_PROCESS_CMD(set, cluster_bool_resp);
}
/* }}} */

/* {{{ proto bool RedisCluster::setex(string key, string value, int expiry) */ 
PHP_METHOD(RedisCluster, setex) {
    CLUSTER_PROCESS_KW_CMD("SETEX", redis_gen_setex_cmd, cluster_bool_resp);
}
/* }}} */

/* {{{ proto bool RedisCluster::psetex(string key, string value, int expiry) */
PHP_METHOD(RedisCluster, psetex) {
    CLUSTER_PROCESS_KW_CMD("PSETEX", redis_gen_setex_cmd, cluster_bool_resp);
}
/* }}} */

/* {{{ proto bool RedisCluster::setnx(string key, string value) */
PHP_METHOD(RedisCluster, setnx) {
    CLUSTER_PROCESS_KW_CMD("SETNX", redis_gen_kv_cmd, cluster_1_resp);
}
/* }}} */

/* {{{ proto string RedisCluster::getSet(string key, string value) */
PHP_METHOD(RedisCluster, getset) {
    CLUSTER_PROCESS_KW_CMD("GETSET", redis_gen_kv_cmd, cluster_bulk_resp);
}
/* }}} */

/* {{{ proto int RedisCluster::exists(string key) */
PHP_METHOD(RedisCluster, exists) {
    CLUSTER_PROCESS_KW_CMD("EXISTS", redis_gen_key_cmd, cluster_int_resp);
}
/* }}} */

/* {{{ proto array Redis::keys(string pattern) */
PHP_METHOD(RedisCluster, keys) {
    // TODO: Figure out how to implement this, as we may want to send it across
    // all nodes (although that seems dangerous), or ask for a specified slot.
    //CLUSTER_PROCESS_KW_CMD("KEYS", redis_gen_key_cmd, cluster_multibulk_resp);
    zend_throw_exception(redis_cluster_exception_ce, 
        "KEYS command not implemented", 0 TSRMLS_CC);
}
/* }}} */

/* {{{ proto int RedisCluster::type(string key) */
PHP_METHOD(RedisCluster, type) {
    CLUSTER_PROCESS_KW_CMD("TYPE", redis_gen_key_cmd, cluster_type_resp);
}
/* }}} */

/* {{{ proto string RedisCluster::pop(string key) */
PHP_METHOD(RedisCluster, lpop) {
    CLUSTER_PROCESS_KW_CMD("LPOP", redis_gen_key_cmd, cluster_bulk_resp);
}
/* }}} */

/* {{{ proto string RedisCluster::rpop(string key) */
PHP_METHOD(RedisCluster, rpop) {
    CLUSTER_PROCESS_KW_CMD("RPOP", redis_gen_key_cmd, cluster_bulk_resp);
}
/* }}} */

/* {{{ proto string RedisCluster::spop(string key) */
PHP_METHOD(RedisCluster, spop) {
    CLUSTER_PROCESS_KW_CMD("SPOP", redis_gen_key_cmd, cluster_bulk_resp);
}
/* }}} */

/* {{{ proto string RedisCluster::strlen(string key) */
PHP_METHOD(RedisCluster, strlen) {
    CLUSTER_PROCESS_KW_CMD("STRLEN", redis_gen_key_cmd, cluster_bulk_resp);
}

/* vim: set tabstop=4 softtabstops=4 noexpandtab shiftwidth=4: */
