// bson.c
/**
 *  Copyright 2009 10gen, Inc.
 * 
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <php.h>

#ifdef WIN32
#  include <memory.h>
#  ifndef int64_t
     typedef __int64 int64_t;
#  endif
#endif

#include "php_mongo.h"
#include "bson.h"
#include "mongo_types.h"

extern zend_class_entry *mongo_ce_BinData,
  *mongo_ce_Code,
  *mongo_ce_Date,
  *mongo_ce_Id,
  *mongo_ce_Regex,
  *mongo_ce_Timestamp,
  *mongo_ce_MinKey,
  *mongo_ce_MaxKey,
  *mongo_ce_Exception;

ZEND_EXTERN_MODULE_GLOBALS(mongo);

static int prep_obj_for_db(buffer *buf, HashTable *array TSRMLS_DC);
#if ZEND_MODULE_API_NO >= 20090115
static int apply_func_args_wrapper(void **data TSRMLS_DC, int num_args, va_list args, zend_hash_key *key);
#else
static int apply_func_args_wrapper(void **data, int num_args, va_list args, zend_hash_key *key);
#endif /* ZEND_MODULE_API_NO >= 20090115 */


static int prep_obj_for_db(buffer *buf, HashTable *array TSRMLS_DC) {
  zval temp, **data, *newid;

  // if _id field doesn't exist, add it
  if (zend_hash_find(array, "_id", 4, (void**)&data) == FAILURE) {
    // create new MongoId
    MAKE_STD_ZVAL(newid);
    object_init_ex(newid, mongo_ce_Id);
    MONGO_METHOD(MongoId, __construct, &temp, newid);

    // add to obj
    zend_hash_add(array, "_id", 4, &newid, sizeof(zval*), NULL);

    // set to data
    data = &newid;
  }

  php_mongo_serialize_element("_id", data, buf, 0 TSRMLS_CC);

  return SUCCESS;
}


// serialize a zval
int zval_to_bson(buffer *buf, HashTable *hash, int prep TSRMLS_DC) {
  uint start;
  int num = 0;

  // check buf size
  if(BUF_REMAINING <= 5) {
    resize_buf(buf, 5);
  }

  // keep a record of the starting position
  // as an offset, in case the memory is resized
  start = buf->pos-buf->start;

  // skip first 4 bytes to leave room for size
  buf->pos += INT_32;

  if (zend_hash_num_elements(hash) > 0) {
    if (prep) {
      prep_obj_for_db(buf, hash TSRMLS_CC);
      num++;
    }
  
#if ZEND_MODULE_API_NO >= 20090115
    zend_hash_apply_with_arguments(hash TSRMLS_CC, (apply_func_args_t)apply_func_args_wrapper, 3, buf, prep, &num);
#else
    zend_hash_apply_with_arguments(hash, (apply_func_args_t)apply_func_args_wrapper, 4, buf, prep, &num TSRMLS_CC);
#endif /* ZEND_MODULE_API_NO >= 20090115 */
  }

  php_mongo_serialize_null(buf);
  php_mongo_serialize_size(buf->start+start, buf);
  return num;
}

#if ZEND_MODULE_API_NO >= 20090115
static int apply_func_args_wrapper(void **data TSRMLS_DC, int num_args, va_list args, zend_hash_key *key)
#else
static int apply_func_args_wrapper(void **data, int num_args, va_list args, zend_hash_key *key)
#endif /* ZEND_MODULE_API_NO >= 20090115 */
{
  int retval;
  char *name;

  buffer *buf = va_arg(args, buffer*);
  int prep = va_arg(args, int);
  int *num = va_arg(args, int*);

#if ZEND_MODULE_API_NO < 20090115
  void ***tsrm_ls = va_arg(args, void***);
#endif /* ZEND_MODULE_API_NO < 20090115 */

  if (key->nKeyLength) {
    return php_mongo_serialize_element(key->arKey, (zval**)data, buf, prep TSRMLS_CC);
  }

  spprintf(&name, 0, "%ld", key->h);
  retval = php_mongo_serialize_element(name, (zval**)data, buf, prep TSRMLS_CC);
  efree(name);

  // if the key is a number in ascending order, we're still
  // dealing with an array, not an object, so increase the count
  if (key->h == (unsigned int)*num) {
    (*num)++;
  }

  return retval;
}

int php_mongo_serialize_element(char *name, zval **data, buffer *buf, int prep TSRMLS_DC) {
  int name_len = strlen(name);

  if (prep && strcmp(name, "_id") == 0) {
    return ZEND_HASH_APPLY_KEEP;
  }

  switch (Z_TYPE_PP(data)) {
  case IS_NULL:
    php_mongo_set_type(buf, BSON_NULL);
    php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
    break;
  case IS_LONG:
    php_mongo_set_type(buf, BSON_INT);
    php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
    php_mongo_serialize_int(buf, Z_LVAL_PP(data));
    break;
  case IS_DOUBLE:
    php_mongo_set_type(buf, BSON_DOUBLE);
    php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
    php_mongo_serialize_double(buf, Z_DVAL_PP(data));
    break;
  case IS_BOOL:
    php_mongo_set_type(buf, BSON_BOOL);
    php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
    php_mongo_serialize_bool(buf, Z_BVAL_PP(data));
    break;
  case IS_STRING: {
    php_mongo_set_type(buf, BSON_STRING);
    php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);

    php_mongo_serialize_int(buf, Z_STRLEN_PP(data)+1);
    php_mongo_serialize_string(buf, Z_STRVAL_PP(data), Z_STRLEN_PP(data));
    break;
  }
  case IS_ARRAY: {
    int num;

    // if we realloc, we need an offset, not an abs pos (phew)
    int type_offset = buf->pos-buf->start;
    // skip type until we know whether it was an array or an object
    buf->pos++;

    //serialize
    php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
    num = zval_to_bson(buf, Z_ARRVAL_PP(data), NO_PREP TSRMLS_CC);

    // now go back and set the type bit
    //php_mongo_set_type(buf, BSON_ARRAY);
    if (num == zend_hash_num_elements(Z_ARRVAL_PP(data))) {
      buf->start[type_offset] = BSON_ARRAY;
    }
    else {
      buf->start[type_offset] = BSON_OBJECT;
    }

    break;
  }
  case IS_OBJECT: {
    zend_class_entry *clazz = Z_OBJCE_PP( data );
    /* check for defined classes */
    // MongoId
    if(clazz == mongo_ce_Id) {
      mongo_id *id;

      php_mongo_set_type(buf, BSON_OID);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
      id = (mongo_id*)zend_object_store_get_object(*data TSRMLS_CC);
      if (!id->id) {
	return ZEND_HASH_APPLY_KEEP;
      }

      php_mongo_serialize_bytes(buf, id->id, OID_SIZE);
    }
    // MongoDate
    else if (clazz == mongo_ce_Date) {
      php_mongo_set_type(buf, BSON_DATE);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
      php_mongo_serialize_date(buf, *data TSRMLS_CC);
    }
    // MongoRegex
    else if (clazz == mongo_ce_Regex) {
      php_mongo_set_type(buf, BSON_REGEX);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
      php_mongo_serialize_regex(buf, *data TSRMLS_CC);
    }
    // MongoCode
    else if (clazz == mongo_ce_Code) {
      php_mongo_set_type(buf, BSON_CODE);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
      php_mongo_serialize_code(buf, *data TSRMLS_CC);
    }
    // MongoBin
    else if (clazz == mongo_ce_BinData) {
      php_mongo_set_type(buf, BSON_BINARY);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
      php_mongo_serialize_bin_data(buf, *data TSRMLS_CC);
    }
    // MongoTimestamp
    else if (clazz == mongo_ce_Timestamp) {
      php_mongo_set_type(buf, BSON_TIMESTAMP);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
      php_mongo_serialize_ts(buf, *data TSRMLS_CC);
    }
    else if (clazz == mongo_ce_MinKey) {
      php_mongo_set_type(buf, BSON_MINKEY);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
    }
    else if (clazz == mongo_ce_MaxKey) {
      php_mongo_set_type(buf, BSON_MAXKEY);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);
    }
    // serialize a normal obj
    else {
      HashTable *hash = Z_OBJPROP_PP(data);

      // go through the k/v pairs and serialize them
      php_mongo_set_type(buf, BSON_OBJECT);
      php_mongo_serialize_key(buf, name, name_len, prep TSRMLS_CC);

      zval_to_bson(buf, hash, NO_PREP TSRMLS_CC);
    } 
    break;
  }
  }

  return ZEND_HASH_APPLY_KEEP;
}

int resize_buf(buffer *buf, int size) {
  int total = buf->end - buf->start;
  int used = buf->pos - buf->start;

  total = total < GROW_SLOWLY ? total*2 : total+INITIAL_BUF_SIZE;
  while (total-used < size) {
    total += size;
  }

  buf->start = (unsigned char*)erealloc(buf->start, total);
  buf->pos = buf->start + used;
  buf->end = buf->start + total;
  return total;
}

/*
 * create a bson date
 *
 * type: 9
 * 8 bytes of ms since the epoch
 */
void php_mongo_serialize_date(buffer *buf, zval *date TSRMLS_DC) {
  int64_t ms;
  zval *sec = zend_read_property(mongo_ce_Date, date, "sec", 3, 0 TSRMLS_CC);
  zval *usec = zend_read_property(mongo_ce_Date, date, "usec", 4, 0 TSRMLS_CC);
  
  ms = ((int64_t)Z_LVAL_P(sec) * 1000) + ((int64_t)Z_LVAL_P(usec) / 1000);
  php_mongo_serialize_long(buf, ms);
}

/*
 * create a bson regex
 *
 * type: 11
 * cstring cstring
 */
void php_mongo_serialize_regex(buffer *buf, zval *regex TSRMLS_DC) {
  zval *z;

  z = zend_read_property(mongo_ce_Regex, regex, "regex", 5, 0 TSRMLS_CC);
  php_mongo_serialize_string(buf, Z_STRVAL_P(z), Z_STRLEN_P(z));
  z = zend_read_property(mongo_ce_Regex, regex, "flags", 5, 0 TSRMLS_CC);
  php_mongo_serialize_string(buf, Z_STRVAL_P(z), Z_STRLEN_P(z));
}

/*
 * create a bson code with scope
 *
 * type: 15
 * 4 bytes total size
 * 4 bytes cstring size + NULL
 * cstring
 * bson object scope
 */
void php_mongo_serialize_code(buffer *buf, zval *code TSRMLS_DC) {
  uint start;
  zval *zid;

  // save spot for size
  start = buf->pos-buf->start;
  buf->pos += INT_32;
  zid = zend_read_property(mongo_ce_Code, code, "code", 4, NOISY TSRMLS_CC);
  // string size
  php_mongo_serialize_int(buf, Z_STRLEN_P(zid)+1);
  // string
  php_mongo_serialize_string(buf, Z_STRVAL_P(zid), Z_STRLEN_P(zid));
  // scope
  zid = zend_read_property(mongo_ce_Code, code, "scope", 5, NOISY TSRMLS_CC);
  zval_to_bson(buf, HASH_P(zid), NO_PREP TSRMLS_CC);
  
  // get total size
  php_mongo_serialize_size(buf->start+start, buf);
}

/*
 * create bson binary data
 *
 * type: 5
 * 4 bytes: length of bindata
 * 1 byte: bindata type
 * bindata
 */
void php_mongo_serialize_bin_data(buffer *buf, zval *bin TSRMLS_DC) {
  zval *zbin, *ztype;

  zbin = zend_read_property(mongo_ce_BinData, bin, "bin", 3, 0 TSRMLS_CC);
  ztype = zend_read_property(mongo_ce_BinData, bin, "type", 4, 0 TSRMLS_CC);

  /* 
   * type 2 has the redundant structure:
   *
   * |------|--|-------==========|
   *  length 02 length   bindata
   *
   *   - 4 bytes: length of bindata (+4 for length below)
   *   - 1 byte type (0x02)
   *   - N bytes: 4 bytes of length of the following bindata + bindata
   *
   */

  if (Z_LVAL_P(ztype) == 2) {
    // length
    php_mongo_serialize_int(buf, Z_STRLEN_P(zbin)+4);
    // 02
    php_mongo_serialize_byte(buf, 2);
    // length
    php_mongo_serialize_int(buf, Z_STRLEN_P(zbin));
  }
  /* other types have
   *
   * |------|--|==========|
   *  length     bindata
   *        type
   */
  else {
    // length
    php_mongo_serialize_int(buf, Z_STRLEN_P(zbin));
    // type
    php_mongo_serialize_byte(buf, (unsigned char)Z_LVAL_P(ztype));
  }

  // bindata
  php_mongo_serialize_bytes(buf, Z_STRVAL_P(zbin), Z_STRLEN_P(zbin));
}

/*
 * create bson timestamp
 *
 * type: 17
 * 4 bytes seconds since epoch
 * 4 bytes increment
 */
void php_mongo_serialize_ts(buffer *buf, zval *time TSRMLS_DC) {
  zval *ts, *inc;

  ts = zend_read_property(mongo_ce_Timestamp, time, "sec", strlen("sec"), NOISY TSRMLS_CC);
  inc = zend_read_property(mongo_ce_Timestamp, time, "inc", strlen("inc"), NOISY TSRMLS_CC);

  php_mongo_serialize_int(buf, Z_LVAL_P(ts));
  php_mongo_serialize_int(buf, Z_LVAL_P(inc));
}

void php_mongo_serialize_byte(buffer *buf, char b) {
  if(BUF_REMAINING <= 1) {
    resize_buf(buf, 1);
  }
  *(buf->pos) = b;
  buf->pos += 1;
}

void php_mongo_serialize_bytes(buffer *buf, char *str, int str_len) {
  if(BUF_REMAINING <= str_len) {
    resize_buf(buf, str_len);
  }
  memcpy(buf->pos, str, str_len);
  buf->pos += str_len;
}

void php_mongo_serialize_string(buffer *buf, char *str, int str_len) {
  if(BUF_REMAINING <= str_len+1) {
    resize_buf(buf, str_len+1);
  }

  memcpy(buf->pos, str, str_len);
  // add \0 at the end of the string
  buf->pos[str_len] = 0;
  buf->pos += str_len + 1;
}

void php_mongo_serialize_int(buffer *buf, int num) {
  if(BUF_REMAINING <= INT_32) {
    resize_buf(buf, INT_32);
  }
  // use mongo_memcpy to deal with big-endianness
  mongo_memcpy(buf->pos, &num, INT_32);
  buf->pos += INT_32;
}

void php_mongo_serialize_long(buffer *buf, int64_t num) {
  if(BUF_REMAINING <= INT_64) {
    resize_buf(buf, INT_64);
  }
  // use mongo_memcpy to deal with big-endianness
  mongo_memcpy(buf->pos, &num, INT_64);
  buf->pos += INT_64;
}

void php_mongo_serialize_double(buffer *buf, double num) {
  if(BUF_REMAINING <= INT_64) {
    resize_buf(buf, INT_64);
  }
  // use mongo_memcpy to deal with big-endianness
  mongo_memcpy(buf->pos, &num, DOUBLE_64);
  buf->pos += DOUBLE_64;
}

/* 
 * prep == true
 *    we are inserting, so keys can't have .s in them
 */
void php_mongo_serialize_key(buffer *buf, char *str, int str_len, int prep TSRMLS_DC) {
  if(BUF_REMAINING <= str_len+1) {
    resize_buf(buf, str_len+1);
  }

  if (prep && (strchr(str, '.') != 0)) {
    zend_error(E_ERROR, "invalid key name: [%s]", str);
  }

  if (MonGlo(cmd_char) && strchr(str, MonGlo(cmd_char)[0]) == str) {
    *(buf->pos) = '$';
    memcpy(buf->pos+1, str+1, str_len-1);
  }
  else {
    memcpy(buf->pos, str, str_len);
  }

  // add \0 at the end of the string
  buf->pos[str_len] = 0;
  buf->pos += str_len + 1;
}

/*
 * replaces collection names starting with MonGlo(cmd_char)
 * with the '$' character.
 *
 * TODO: this doesn't handle main.$oplog-type situations (if
 * MonGlo(cmd_char) is set)
 */
void php_mongo_serialize_ns(buffer *buf, char *str TSRMLS_DC) {
  char *collection = strchr(str, '.')+1;

  if(BUF_REMAINING <= (int)strlen(str)+1) {
    resize_buf(buf, strlen(str)+1);
  }

  if (MonGlo(cmd_char) && strchr(collection, MonGlo(cmd_char)[0]) == collection) {
    char *tmp = buf->pos;
    memcpy(buf->pos, str, collection-str);
    buf->pos += collection-str;
    *(buf->pos) = '$';
    memcpy(buf->pos+1, collection+1, strlen(collection)-1);
    buf->pos[strlen(collection)] = 0;
    buf->pos += strlen(collection) + 1;
  }
  else {
    memcpy(buf->pos, str, strlen(str));
    buf->pos[strlen(str)] = 0;
    buf->pos += strlen(str) + 1;
  }
}


/* the position is not increased, we are just filling
 * in the first 4 bytes with the size.
 */
void php_mongo_serialize_size(unsigned char *start, buffer *buf) {
  int total = buf->pos - start;
  // use mongo_memcpy to deal with big-endianness
  mongo_memcpy(start, &total, INT_32);

}


char* bson_to_zval(char *buf, HashTable *result TSRMLS_DC) {
  /* 
   * buf_start is used for debugging
   *
   * if the deserializer runs into bson it can't
   * parse, it will dump the bytes to that point.
   *
   * we lose buf's position as we iterate, so we
   * need buf_start to save it. 
   */
  char *buf_start = buf;
  char type;

  // for size
  buf += INT_32;
  
  while ((type = *buf++) != 0) {
    char *name;
    zval *value;
    
    name = buf;
    // get past field name
    buf += strlen(buf) + 1;

    MAKE_STD_ZVAL(value);
    
    // get value
    switch(type) {
    case BSON_OID: {
      mongo_id *this_id;

      object_init_ex(value, mongo_ce_Id);

      this_id = (mongo_id*)zend_object_store_get_object(value TSRMLS_CC);
      this_id->id = estrndup(buf, OID_SIZE);

      buf += OID_SIZE;
      break;
    }
    case BSON_DOUBLE: {
#if PHP_C_BIGENDIAN
      char d[8];
      mongo_memcpy(d, buf, 8);
      ZVAL_DOUBLE(value, *(double*)d);
#else
      ZVAL_DOUBLE(value, *(double*)buf);
#endif
      buf += DOUBLE_64;
      break;
    }
    case BSON_STRING: {
      // len includes \0
      int len = MONGO_INT(*((int*)buf));
      buf += INT_32;

      ZVAL_STRINGL(value, buf, len-1, 1);
      buf += len;
      break;
    }
    case BSON_OBJECT: 
    case BSON_ARRAY: {
      array_init(value);
      buf = bson_to_zval(buf, Z_ARRVAL_P(value) TSRMLS_CC);
      break;
    }
    case BSON_BINARY: {
      char type;

      int len = MONGO_INT(*(int*)buf);
      buf += INT_32;

      type = *buf++;

      /* If the type is 2, check if the binary data
       * is prefixed by its length.
       * 
       * There is an infinitesimally small chance that
       * the first four bytes will happen to be the
       * length of the rest of the string.  In this
       * case, the data will be corrupted.
       */
      if ((int)type == 2) {
        int len2 = MONGO_INT(*(int*)buf);
        /* if the lengths match, the data is to spec,
         * so we use len2 as the true length.
         */
        if (len2 == len - 4) {
          len = len2;
          buf += INT_32;
        }
      }

      object_init_ex(value, mongo_ce_BinData);

      add_property_stringl(value, "bin", buf, len, DUP);
      buf += len;

      add_property_long(value, "type", type);
      break;
    }
    case BSON_BOOL: {
      char d = *buf++;
      ZVAL_BOOL(value, d);
      break;
    }
    case BSON_UNDEF:
    case BSON_NULL: {
      ZVAL_NULL(value);
      break;
    }
    case BSON_INT: {
      ZVAL_LONG(value, MONGO_INT(*((int*)buf)));
      buf += INT_32;
      break;
    }
    case BSON_LONG: {
#if PHP_C_BIGENDIAN
      int64_t li;
      char *p = &li;
      mongo_memcpy(p, buf, INT_64);
      ZVAL_DOUBLE(value, (double)li);
#else
      ZVAL_DOUBLE(value, (double)*((int64_t*)buf));
#endif
      buf += INT_64;
      break;
    }
    case BSON_DATE: {
#if PHP_C_BIGENDIAN
      int64_t d;
      char *p = &d;
      mongo_memcpy(p, buf, INT_64);
#else
      int64_t d = *((int64_t*)buf);
#endif
      buf += INT_64;
      
      object_init_ex(value, mongo_ce_Date);

      add_property_long(value, "sec", (long)(d/1000));
      add_property_long(value, "usec", (long)((d*1000)%1000000));

      break;
    }
    case BSON_REGEX: {
      char *regex, *flags;
      int regex_len, flags_len;

      regex = buf;
      regex_len = strlen(buf);
      buf += regex_len+1;

      flags = buf;
      flags_len = strlen(buf);
      buf += flags_len+1;

      object_init_ex(value, mongo_ce_Regex);

      add_property_stringl(value, "regex", regex, regex_len, 1);
      add_property_stringl(value, "flags", flags, flags_len, 1);

      break;
    }
    case BSON_CODE: 
    case BSON_CODE__D: {
      zval *zcope;
      int code_len;
      char *code;

      object_init_ex(value, mongo_ce_Code);
      // initialize scope array
      MAKE_STD_ZVAL(zcope);
      array_init(zcope);

      // CODE has a useless total size field
      if (type == BSON_CODE) {
        buf += INT_32;
      }

      // length of code (includes \0)
      code_len = MONGO_INT(*(int*)buf);
      buf += INT_32;

      code = buf;
      buf += code_len;

      if (type == BSON_CODE) {
        buf = bson_to_zval(buf, HASH_P(zcope) TSRMLS_CC);
      }

      // exclude \0
      add_property_stringl(value, "code", code, code_len-1, DUP);
      add_property_zval(value, "scope", zcope);

      // add_property_zval creates an extra zcope ref
      zval_ptr_dtor(&zcope);
      break;
    }
    /* DEPRECATED
     * database reference (12)
     *   - 4 bytes ns length (includes trailing \0)
     *   - ns + \0
     *   - 12 bytes MongoId
     * This converts the deprecated, old-style db ref type 
     * into the new type (array('$ref' => ..., $id => ...)).
     */
    case BSON_DBREF: {
      int ns_len;
      char *ns;
      zval *zoid;
      mongo_id *this_id;

      // ns
      ns_len = *(int*)buf;
      buf += INT_32;
      ns = buf;
      buf += ns_len;

      // id
      MAKE_STD_ZVAL(zoid);
      object_init_ex(zoid, mongo_ce_Id);

      this_id = (mongo_id*)zend_object_store_get_object(zoid TSRMLS_CC);
      this_id->id = estrndup(buf, OID_SIZE);

      buf += OID_SIZE;

      // put it all together
      array_init(value);
      add_assoc_stringl(value, "$ref", ns, ns_len-1, 1);
      add_assoc_zval(value, "$id", zoid);
      break;
    }
    /* MongoTimestamp (17)
     * 8 bytes total:
     *  - sec: 4 bytes
     *  - inc: 4 bytes
     */
    case BSON_TIMESTAMP: {
      object_init_ex(value, mongo_ce_Timestamp);
      zend_update_property_long(mongo_ce_Timestamp, value, "sec", strlen("sec"), MONGO_INT(*(int*)buf) TSRMLS_CC);
      buf += INT_32;
      zend_update_property_long(mongo_ce_Timestamp, value, "inc", strlen("inc"), MONGO_INT(*(int*)buf) TSRMLS_CC);
      buf += INT_32;
      break;
    }
    /* max key (127)
     * max and min keys are used only for sharding, and 
     * cannot be resaved to the database at the moment
     */
    case BSON_MINKEY: {
      object_init_ex(value, mongo_ce_MinKey);
      break;
    }
    /* min key (0)
     */
    case BSON_MAXKEY: {
      object_init_ex(value, mongo_ce_MaxKey);
      break;
    }
    default: {
      /* if we run into a type we don't recognize, there's
       * either been some corruption or we've messed up on
       * the parsing.  Either way, it's helpful to know the
       * situation that led us here, so this dumps the 
       * buffer up to this point to stdout and returns.  
       *
       * We can't dump any more of the buffer, unfortunately,
       * because we don't keep track of the size.  Besides,
       * if it is corrupt, the size might be messed up, too.
       */
      int i;
      php_printf("type %d not supported\n", type);
      for (i=0; i<buf-buf_start; i++) {
        printf("%d ", buf_start[i]);
      }
      printf("<-- \n");
      // give up, it'll be trouble if we keep going
      return buf;
    }
    }

    zend_symtable_update(result, name, strlen(name)+1, &value, sizeof(zval*), NULL);
  }

  return buf;
}

