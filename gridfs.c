//gridfs.c
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
#  ifndef int64_t
     typedef __int64 int64_t;
#  endif
#endif

#include <zend_exceptions.h>

#include "gridfs.h"
#include "collection.h"
#include "cursor.h"
#include "php_mongo.h"
#include "mongo_types.h"
#include "db.h"

extern zend_class_entry *mongo_ce_DB,
  *mongo_ce_Collection,
  *mongo_ce_Cursor,
  *mongo_ce_Exception,
  *mongo_ce_GridFSException,
  *mongo_ce_Id,
  *mongo_ce_Date,
  *mongo_ce_BinData;

ZEND_EXTERN_MODULE_GLOBALS(mongo);

zend_class_entry *mongo_ce_GridFS = NULL,
  *mongo_ce_GridFSFile = NULL,
  *mongo_ce_GridFSCursor = NULL;


typedef int (*apply_copy_func_t)(void *to, char *from, int len);

static int copy_bytes(void *to, char *from, int len);
static int copy_file(void *to, char *from, int len);
static void add_md5(zval *zfile, zval *zid, mongo_collection *c TSRMLS_DC);

static int apply_to_cursor(zval *cursor, apply_copy_func_t apply_copy_func, void *to TSRMLS_DC);
static int setup_file(FILE *fpp, char *filename TSRMLS_DC);
static int get_chunk_size(zval *array TSRMLS_DC);
static zval* setup_extra(zval *zfile, zval *extra TSRMLS_DC);
static int setup_file_fields(zval *zfile, char *filename, int size TSRMLS_DC);
static int insert_chunk(zval *chunks, zval *zid, int chunk_num, char *buf, int chunk_size TSRMLS_DC);


PHP_METHOD(MongoGridFS, __construct) {
  zval *zdb, *files = 0, *chunks = 0, *zchunks, *zidx;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|zz", &zdb, mongo_ce_DB, &files, &chunks) == FAILURE) {
    return;
  }

  if (!files && !chunks) {
    MAKE_STD_ZVAL(files);
    ZVAL_STRING(files, "fs.files", 1);
    MAKE_STD_ZVAL(chunks);
    ZVAL_STRING(chunks, "fs.chunks", 1);
  }
  else if (!chunks) {
    zval *temp_file;
    char *temp;

    MAKE_STD_ZVAL(chunks);
    spprintf(&temp, 0, "%s.chunks", Z_STRVAL_P(files));
    ZVAL_STRING(chunks, temp, 0);

    MAKE_STD_ZVAL(temp_file);
    spprintf(&temp, 0, "%s.files", Z_STRVAL_P(files));
    ZVAL_STRING(temp_file, temp, 0);
    files = temp_file;
  }
  else {
    convert_to_string(files);
    zval_add_ref(&files);
    convert_to_string(chunks);
    zval_add_ref(&chunks);
  }

  // create files collection
  MONGO_METHOD2(MongoCollection, __construct, return_value, getThis(), zdb, files);

  // create chunks collection
  MAKE_STD_ZVAL(zchunks);
  object_init_ex(zchunks, mongo_ce_Collection);
  MONGO_METHOD2(MongoCollection, __construct, return_value, zchunks, zdb, chunks);
  
  // add chunks collection as a property
  zend_update_property(mongo_ce_GridFS, getThis(), "chunks", strlen("chunks"), zchunks TSRMLS_CC);
  
  // ensure index on chunks.n
  MAKE_STD_ZVAL(zidx);
  array_init(zidx);
  add_assoc_long(zidx, "files_id", 1);
  add_assoc_long(zidx, "n", 1);

  MONGO_METHOD1(MongoCollection, ensureIndex, return_value, zchunks, zidx);

  zend_update_property(mongo_ce_GridFS, getThis(), "filesName", strlen("filesName"), files TSRMLS_CC);
  zend_update_property(mongo_ce_GridFS, getThis(), "chunksName", strlen("chunksName"), chunks TSRMLS_CC);

  // cleanup
  zval_ptr_dtor(&zchunks);
  zval_ptr_dtor(&zidx);

  zval_ptr_dtor(&files);
  zval_ptr_dtor(&chunks);
}


PHP_METHOD(MongoGridFS, drop) {
  zval *temp;
  zval *zchunks = zend_read_property(mongo_ce_GridFS, getThis(), "chunks", strlen("chunks"), NOISY TSRMLS_CC);

  MAKE_STD_ZVAL(temp);
  MONGO_METHOD(MongoCollection, drop, temp, zchunks);
  zval_ptr_dtor(&temp);
  
  MONGO_METHOD(MongoCollection, drop, return_value, getThis());
}

PHP_METHOD(MongoGridFS, find) {
  zval temp;
  zval *zquery = 0, *zfields = 0;
  mongo_collection *c;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|zz", &zquery, &zfields) == FAILURE) {
    return;
  }

  if (!zquery) {
    MAKE_STD_ZVAL(zquery);
    array_init(zquery);
  }
  else {
    zval_add_ref(&zquery);
  }

  if (!zfields) {
    MAKE_STD_ZVAL(zfields);
    array_init(zfields);
  }
  else {
    zval_add_ref(&zfields);
  }

  object_init_ex(return_value, mongo_ce_GridFSCursor);

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);
  MONGO_CHECK_INITIALIZED(c->ns, MongoGridFS);

  MONGO_METHOD5(MongoGridFSCursor, __construct, &temp, return_value, getThis(), c->db->link, c->ns, zquery, zfields);

  zval_ptr_dtor(&zquery);
  zval_ptr_dtor(&zfields);
}


static int get_chunk_size(zval *array TSRMLS_DC) {
  zval **zchunk_size = 0;

  if (zend_hash_find(HASH_P(array), "chunkSize", strlen("chunkSize")+1, (void**)&zchunk_size) == FAILURE) {
    add_assoc_long(array, "chunkSize", MonGlo(chunk_size));
    return MonGlo(chunk_size);
  }

  convert_to_long(*zchunk_size);
  return Z_LVAL_PP(zchunk_size) > 0 ? 
    Z_LVAL_PP(zchunk_size) :
    MonGlo(chunk_size);
}


static int setup_file(FILE *fp, char *filename TSRMLS_DC) {
  int size = 0;

  // try to open the file
  if (!fp) {
    zend_throw_exception_ex(mongo_ce_GridFSException, 0 TSRMLS_CC, "could not open file %s", filename);
    return FAILURE;
  }

  // get size
  fseek(fp, 0, SEEK_END);
  size = ftell(fp);
  if (size >= 0xffffffff) {
    zend_throw_exception_ex(mongo_ce_GridFSException, 0 TSRMLS_CC, "file %s is too large: %ld bytes", filename, size);
    fclose(fp);
    return FAILURE;
  }

  // reset file ptr
  fseek(fp, 0, SEEK_SET);

  return size;
}

static zval* setup_extra(zval *zfile, zval *extra TSRMLS_DC) {
  zval temp;
  zval *zid = 0;
  zval **zzid = 0;

  array_init(zfile);

  // add user-defined fields
  if (extra) {
    zval temp;
    zend_hash_merge(HASH_P(zfile), Z_ARRVAL_P(extra), (void (*)(void*))zval_add_ref, &temp, sizeof(zval*), 1);
  }

  // check if we need to add any fields

  // _id
  if (zend_hash_find(HASH_P(zfile), "_id", strlen("_id")+1, (void**)&zzid) == FAILURE) {
    // create an id for the file
    MAKE_STD_ZVAL(zid);
    object_init_ex(zid, mongo_ce_Id);
    MONGO_METHOD(MongoId, __construct, &temp, zid);

    add_assoc_zval(zfile, "_id", zid);
  }
  else {
    zid = *zzid;
  }
  return zid;
}

/* Use the db command to get the md5 hash of the inserted chunks
 *
 * $db->command(array(filemd5 => $fileId, "root" => $ns));
 *
 * adds the response to zfile as the "md5" field.
 *
 */
static void add_md5(zval *zfile, zval *zid, mongo_collection *c TSRMLS_DC) {
  if (!zend_hash_exists(HASH_P(zfile), "md5", strlen("md5")+1)) {
    zval *data = 0, *response = 0, **md5 = 0;

    // get the prefix
    int prefix_len = strchr(Z_STRVAL_P(c->name), '.') - Z_STRVAL_P(c->name);
    char *prefix = estrndup(Z_STRVAL_P(c->name), prefix_len);

    // create command
    MAKE_STD_ZVAL(data);
    array_init(data);

    add_assoc_zval(data, "filemd5", zid);
    zval_add_ref(&zid);
    add_assoc_stringl(data, "root", prefix, prefix_len, 0);

    MAKE_STD_ZVAL(response);

    // run command
    MONGO_CMD(response, c->parent); 

    // make sure there wasn't an error
    if (zend_hash_find(HASH_P(response), "md5", strlen("md5")+1, (void**)&md5) == SUCCESS) {
      // add it to zfile
      add_assoc_zval(zfile, "md5", *md5);
      /* 
       * increment the refcount so it isn't cleaned up at 
       * the end of this method
       */
      zval_add_ref(md5);
    }

    // cleanup
    zval_ptr_dtor(&response);
    zval_ptr_dtor(&data);
  }
}

/* 
 * Stores an array of bytes that may not have a filename,
 * such as data from a socket or stream.
 *
 * Still somewhat limited atm, as the string has to fit
 * in memory.  It would be better if it could take a fh
 * or something.
 *
 */
PHP_METHOD(MongoGridFS, storeBytes) {
  char *bytes = 0;
  int bytes_len = 0, chunk_num = 0, chunk_size = 0, global_chunk_size = 0, pos = 0;

  zval temp;
  zval *extra = 0, *zid = 0, *zfile = 0, *chunks = 0;

  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);
  MONGO_CHECK_INITIALIZED(c->ns, MongoGridFS);

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|a", &bytes, &bytes_len, &extra) == FAILURE) {
    return;
  }

  // file array object
  MAKE_STD_ZVAL(zfile);

  // merge extra & zfile and add _id if needed
  zid = setup_extra(zfile, extra TSRMLS_CC);
  // chunkSize
  global_chunk_size = get_chunk_size(zfile TSRMLS_CC);

  // size
  if (!zend_hash_exists(HASH_P(zfile), "length", strlen("length")+1)) {
    add_assoc_long(zfile, "length", bytes_len);
  }

  // insert chunks
  chunks = zend_read_property(mongo_ce_GridFS, getThis(), "chunks", strlen("chunks"), NOISY TSRMLS_CC);
  while (pos < bytes_len) {
    chunk_size = bytes_len-pos >= global_chunk_size ? global_chunk_size : bytes_len-pos;

    insert_chunk(chunks, zid, chunk_num, bytes+pos, chunk_size TSRMLS_CC);
    
    // increment counters
    pos += chunk_size;
    chunk_num++; 
  }

  // now that we've inserted the chunks, use them to calculate the hash
  add_md5(zfile, zid, c TSRMLS_CC);

  // insert file
  MONGO_METHOD1(MongoCollection, insert, &temp, getThis(), zfile);

  zval_add_ref(&zid);
  zval_ptr_dtor(&zfile);

  RETURN_ZVAL(zid, 1, 1);
}

/* add extra fields required for files:
 * - filename
 * - upload date
 * - length
 * these fields are only added if the user hasn't defined them.
 */
static int setup_file_fields(zval *zfile, char *filename, int size TSRMLS_DC) {
  zval temp;

  // filename
  if (!zend_hash_exists(HASH_P(zfile), "filename", strlen("filename")+1)) {
    add_assoc_stringl(zfile, "filename", filename, strlen(filename), DUP);
  }

  // uploadDate
  if (!zend_hash_exists(HASH_P(zfile), "uploadDate", strlen("uploadDate")+1)) {
    // create an id for the file
    zval *upload_date;
    MAKE_STD_ZVAL(upload_date);
    object_init_ex(upload_date, mongo_ce_Date);
    MONGO_METHOD(MongoDate, __construct, &temp, upload_date);

    add_assoc_zval(zfile, "uploadDate", upload_date);
  }

  // size
  if (!zend_hash_exists(HASH_P(zfile), "length", strlen("length")+1)) {
    add_assoc_long(zfile, "length", size);
  }

  return SUCCESS;
}

/* Creates a chunk and adds it to the chunks collection as:
 * array(3) {
 *   files_id => zid
 *   n => chunk_num
 *   data => MongoBinData(buf, chunk_size, type 2)
 * }
 *
 * Clean up should leave:
 * - 1 ref to zid
 * - buf
 */
static int insert_chunk(zval *chunks, zval *zid, int chunk_num, char *buf, int chunk_size TSRMLS_DC) {
  zval temp;
  zval *zchunk, *zbin;

  // create chunk
  MAKE_STD_ZVAL(zchunk);
  array_init(zchunk);

  add_assoc_zval(zchunk, "files_id", zid);
  zval_add_ref(&zid); // zid->refcount = 2
  add_assoc_long(zchunk, "n", chunk_num);

  // create MongoBinData object
  MAKE_STD_ZVAL(zbin);
  object_init_ex(zbin, mongo_ce_BinData);
  add_property_stringl(zbin, "bin", buf, chunk_size, DUP);
  add_property_long(zbin, "type", 2);

  add_assoc_zval(zchunk, "data", zbin);

  // insert chunk
  MONGO_METHOD1(MongoCollection, insert, &temp, chunks, zchunk);
    
  // increment counters
  zval_ptr_dtor(&zchunk); // zid->refcount = 1

  return SUCCESS;
}


PHP_METHOD(MongoGridFS, storeFile) {
  char *filename = 0;
  int filename_len = 0, chunk_num = 0, chunk_size = 0, global_chunk_size = 0, size = 0, pos = 0;
  FILE *fp = 0;

  zval temp;
  zval *extra = 0, *zid = 0, *zfile = 0, *chunks = 0;

  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);
  MONGO_CHECK_INITIALIZED(c->ns, MongoGridFS);

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|a", &filename, &filename_len, &extra) == FAILURE) {
    return;
  }

  fp = fopen(filename, "rb");
  // no point in continuing if we can't open the file
  if ((size = setup_file(fp, filename TSRMLS_CC)) == FAILURE) {
    return;
  }

  // file array object
  MAKE_STD_ZVAL(zfile);

  // merge extra & zfile and add _id if needed
  zid = setup_extra(zfile, extra TSRMLS_CC);
  setup_file_fields(zfile, filename, size TSRMLS_CC);

  // chunkSize
  global_chunk_size = get_chunk_size(zfile TSRMLS_CC);

  // insert chunks
  chunks = zend_read_property(mongo_ce_GridFS, getThis(), "chunks", strlen("chunks"), NOISY TSRMLS_CC);
  while (pos < size) {
    char *buf;

    chunk_size = size-pos >= global_chunk_size ? global_chunk_size : size-pos;
    buf = (char*)emalloc(chunk_size); 
    if ((int)fread(buf, 1, chunk_size, fp) < chunk_size) {
      zend_throw_exception_ex(mongo_ce_GridFSException, 0 TSRMLS_CC, "error reading file %s", filename);
      return;
    }

    insert_chunk(chunks, zid, chunk_num, buf, chunk_size TSRMLS_CC);
    
    // increment counters
    pos += chunk_size;
    chunk_num++; 

    efree(buf);
  }
  // close file ptr
  fclose(fp);

  add_md5(zfile, zid, c TSRMLS_CC);

  // insert file
  MONGO_METHOD1(MongoCollection, insert, &temp, getThis(), zfile);

  // cleanup
  zval_add_ref(&zid);
  zval_ptr_dtor(&zfile);

  RETURN_ZVAL(zid, 1, 1);
}

PHP_METHOD(MongoGridFS, findOne) {
  zval *zquery = 0, *file;
  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|z", &zquery) == FAILURE) {
    return;
  }

  if (!zquery) {
    MAKE_STD_ZVAL(zquery);
    array_init(zquery);
  }
  else if (Z_TYPE_P(zquery) != IS_ARRAY) {
    zval *temp;

    convert_to_string(zquery);

    MAKE_STD_ZVAL(temp);
    array_init(temp);
    add_assoc_string(temp, "filename", Z_STRVAL_P(zquery), 1);

    zquery = temp;
  }
  else {
    zval_add_ref(&zquery);
  }

  MAKE_STD_ZVAL(file);
  MONGO_METHOD1(MongoCollection, findOne, file, getThis(), zquery);

  if (Z_TYPE_P(file) == IS_NULL) {
    RETVAL_ZVAL(file, 0, 1);
  }
  else {
    zval temp;

    object_init_ex(return_value, mongo_ce_GridFSFile);
    MONGO_METHOD2(MongoGridFSFile, __construct, &temp, return_value, getThis(), file);
  }

  zval_ptr_dtor(&file);
  zval_ptr_dtor(&zquery);
}


PHP_METHOD(MongoGridFS, remove) {
  zval zjust_one;
  zval *criteria = 0, *zfields, *zcursor, *chunks, *next;
  zend_bool just_one = 0;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|zb", &criteria, &just_one) == FAILURE) {
    return;
  }

  if (!criteria) {
    MAKE_STD_ZVAL(criteria);
    array_init(criteria);
  }
  else {
    zval_add_ref(&criteria);
  }

  // { _id : 1 }
  MAKE_STD_ZVAL(zfields);
  array_init(zfields);
  add_assoc_long(zfields, "_id", 1);

  // cursor = db.fs.files.find(criteria, {_id : 1});
  MAKE_STD_ZVAL(zcursor);
  MONGO_METHOD2(MongoCollection, find, zcursor, getThis(), criteria, zfields);

  zval_ptr_dtor(&zfields);

  chunks = zend_read_property(mongo_ce_GridFS, getThis(), "chunks", strlen("chunks"), NOISY TSRMLS_CC);

  MAKE_STD_ZVAL(next);
  MONGO_METHOD(MongoCursor, getNext, next, zcursor);

  while (Z_TYPE_P(next) != IS_NULL) {
    zval **id;
    zval *temp;

    if (zend_hash_find(HASH_P(next), "_id", 4, (void**)&id) == FAILURE) {
      // uh oh
      continue;
    }

    MAKE_STD_ZVAL(temp);
    array_init(temp);
    zval_add_ref(id);
    add_assoc_zval(temp, "files_id", *id);

    MONGO_METHOD1(MongoCollection, remove, return_value, chunks, temp);

    zval_ptr_dtor(&temp);
    zval_ptr_dtor(&next);
    MAKE_STD_ZVAL(next);
    MONGO_METHOD(MongoCursor, getNext, next, zcursor);
  }
  zval_ptr_dtor(&next);
  zval_ptr_dtor(&zcursor);

  Z_TYPE(zjust_one) = IS_BOOL;
  zjust_one.value.lval = just_one;

  MONGO_METHOD2(MongoCollection, remove, return_value, getThis(), criteria, &zjust_one);

  zval_ptr_dtor(&criteria);
}

PHP_METHOD(MongoGridFS, storeUpload) {
  zval *filename, *h, *extra;
  zval **file, **temp;
  char *new_name = 0;
  int new_len = 0;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|s", &filename, &new_name, &new_len) == FAILURE) {
    return;
  }
  convert_to_string(filename);

  h = PG(http_globals)[TRACK_VARS_FILES];
  if (zend_hash_find(Z_ARRVAL_P(h), Z_STRVAL_P(filename), Z_STRLEN_P(filename)+1, (void**)&file) == FAILURE) {
    zend_throw_exception_ex(mongo_ce_GridFSException, 0 TSRMLS_CC, "could not find uploaded file %s", Z_STRVAL_P(filename));
    return;
  }

  zend_hash_find(Z_ARRVAL_PP(file), "tmp_name", strlen("tmp_name")+1, (void**)&temp);
  convert_to_string(*temp);

  if (!new_name) {
    zval **n;
    zend_hash_find(Z_ARRVAL_PP(file), "name", strlen("name")+1, (void**)&n);
    new_name = Z_STRVAL_PP(n);
  }

  MAKE_STD_ZVAL(extra);
  array_init(extra);
  add_assoc_string(extra, "filename", new_name, 1);

  MONGO_METHOD2(MongoGridFS, storeFile, return_value, getThis(), *temp, extra);
  zval_ptr_dtor(&extra);
}


static function_entry MongoGridFS_methods[] = {
  PHP_ME(MongoGridFS, __construct, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFS, drop, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFS, find, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFS, storeFile, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFS, storeBytes, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFS, findOne, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFS, remove, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFS, storeUpload, NULL, ZEND_ACC_PUBLIC)
  {NULL, NULL, NULL}
};

void mongo_init_MongoGridFS(TSRMLS_D) {
  zend_class_entry ce;

  INIT_CLASS_ENTRY(ce, "MongoGridFS", MongoGridFS_methods);
  ce.create_object = php_mongo_collection_new;
  mongo_ce_GridFS = zend_register_internal_class_ex(&ce, mongo_ce_Collection, "MongoCollection" TSRMLS_CC);

  zend_declare_property_null(mongo_ce_GridFS, "chunks", strlen("chunks"), ZEND_ACC_PUBLIC TSRMLS_CC);

  zend_declare_property_null(mongo_ce_GridFS, "filesName", strlen("filesName"), ZEND_ACC_PROTECTED TSRMLS_CC);
  zend_declare_property_null(mongo_ce_GridFS, "chunksName", strlen("chunksName"), ZEND_ACC_PROTECTED TSRMLS_CC);
}


PHP_METHOD(MongoGridFSFile, __construct) {
  zval *gridfs = 0, *file = 0;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Oa", &gridfs, mongo_ce_GridFS, &file) == FAILURE) {
    return;
  }

  zend_update_property(mongo_ce_GridFSFile, getThis(), "gridfs", strlen("gridfs"), gridfs TSRMLS_CC);
  zend_update_property(mongo_ce_GridFSFile, getThis(), "file", strlen("file"), file TSRMLS_CC);
}

PHP_METHOD(MongoGridFSFile, getFilename) {
  zval *file = zend_read_property(mongo_ce_GridFSFile, getThis(), "file", strlen("file"), NOISY TSRMLS_CC);
  zend_hash_find(HASH_P(file), "filename", strlen("filename")+1, (void**)&return_value_ptr);
  RETURN_ZVAL(*return_value_ptr, 1, 0);
}

PHP_METHOD(MongoGridFSFile, getSize) {
  zval *file = zend_read_property(mongo_ce_GridFSFile, getThis(), "file", strlen("file"), NOISY TSRMLS_CC);
  zend_hash_find(HASH_P(file), "length", strlen("length")+1, (void**)&return_value_ptr);
  RETURN_ZVAL(*return_value_ptr, 1, 0);
}

PHP_METHOD(MongoGridFSFile, write) {
  char *filename = 0;
  int filename_len, total = 0;
  zval *gridfs, *file, *chunks, *n, *query, *cursor, *sort;
  zval **id;
  FILE *fp;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &filename, &filename_len) == FAILURE) {
    return;
  }

  gridfs = zend_read_property(mongo_ce_GridFSFile, getThis(), "gridfs", strlen("gridfs"), NOISY TSRMLS_CC);
  file = zend_read_property(mongo_ce_GridFSFile, getThis(), "file", strlen("file"), NOISY TSRMLS_CC);

  // make sure that there's an index on chunks so we can sort by chunk num
  chunks = zend_read_property(mongo_ce_GridFS, gridfs, "chunks", strlen("chunks"), NOISY TSRMLS_CC);

  MAKE_STD_ZVAL(n);
  array_init(n);
  add_assoc_long(n, "files_id", 1);
  add_assoc_long(n, "n", 1);

  MONGO_METHOD1(MongoCollection, ensureIndex, return_value, chunks, n);
  zval_ptr_dtor(&n);

  if (!filename) {
    zval **temp;
    zend_hash_find(HASH_P(file), "filename", strlen("filename")+1, (void**)&temp);

    filename = Z_STRVAL_PP(temp);
  }

  
  fp = fopen(filename, "wb");
  if (!fp) {
    zend_throw_exception_ex(mongo_ce_GridFSException, 0 TSRMLS_CC, "could not open destination file %s", filename);
    return;
  }

  zend_hash_find(HASH_P(file), "_id", strlen("_id")+1, (void**)&id);

  MAKE_STD_ZVAL(query);
  array_init(query);
  zval_add_ref(id);
  add_assoc_zval(query, "files_id", *id);

  MAKE_STD_ZVAL(cursor);
  MONGO_METHOD1(MongoCollection, find, cursor, chunks, query);

  MAKE_STD_ZVAL(sort);
  array_init(sort);
  add_assoc_long(sort, "n", 1);

  MONGO_METHOD1(MongoCursor, sort, cursor, cursor, sort);

  if ((total = apply_to_cursor(cursor, copy_file, fp TSRMLS_CC)) == FAILURE) {
    zend_throw_exception(mongo_ce_GridFSException, "error reading chunk of file", 0 TSRMLS_CC);
  }

  fclose(fp);

  zval_ptr_dtor(&cursor);
  zval_ptr_dtor(&sort); 
  zval_ptr_dtor(&query);

  RETURN_LONG(total);
}

PHP_METHOD(MongoGridFSFile, getBytes) {
  zval *file, *gridfs, *chunks, *n, *query, *cursor, *sort, *temp;
  zval **id, **size;
  char *str, *str_ptr;
  int len;

  file = zend_read_property(mongo_ce_GridFSFile, getThis(), "file", strlen("file"), NOISY TSRMLS_CC);
  zend_hash_find(HASH_P(file), "_id", strlen("_id")+1, (void**)&id);

  if (zend_hash_find(HASH_P(file), "length", strlen("length")+1, (void**)&size) == FAILURE) {
    zend_throw_exception(mongo_ce_GridFSException, "couldn't find file size", 0 TSRMLS_CC);
    return;
  }

  // make sure that there's an index on chunks so we can sort by chunk num
  gridfs = zend_read_property(mongo_ce_GridFSFile, getThis(), "gridfs", strlen("gridfs"), NOISY TSRMLS_CC);
  chunks = zend_read_property(mongo_ce_GridFS, gridfs, "chunks", strlen("chunks"), NOISY TSRMLS_CC);

  MAKE_STD_ZVAL(temp);
  MAKE_STD_ZVAL(n);
  array_init(n);
  add_assoc_long(n, "files_id", 1);  
  add_assoc_long(n, "n", 1);  

  MONGO_METHOD1(MongoCollection, ensureIndex, temp, chunks, n);
  zval_ptr_dtor(&n);

  // query for chunks
  MAKE_STD_ZVAL(query);
  array_init(query);
  zval_add_ref(id);
  add_assoc_zval(query, "files_id", *id);

  MAKE_STD_ZVAL(cursor);
  MONGO_METHOD1(MongoCollection, find, cursor, chunks, query);

  MAKE_STD_ZVAL(sort);
  array_init(sort);
  add_assoc_long(sort, "n", 1);

  MONGO_METHOD1(MongoCursor, sort, temp, cursor, sort);
  zval_ptr_dtor(&temp);

  zval_ptr_dtor(&query);
  zval_ptr_dtor(&sort);

  if (Z_TYPE_PP(size) == IS_DOUBLE) {
    len = (int)Z_DVAL_PP(size);
  }
  else { // if Z_TYPE_PP(size) == IS_LONG
    len = Z_LVAL_PP(size);
  }

  str = (char*)emalloc(len + 1);
  str_ptr = str;
  
  if (apply_to_cursor(cursor, copy_bytes, &str TSRMLS_CC) == FAILURE) {
    zend_throw_exception(mongo_ce_GridFSException, "error reading chunk of file", 0 TSRMLS_CC);
  }
  
  zval_ptr_dtor(&cursor);

  str_ptr[len] = '\0';

  RETURN_STRINGL(str_ptr, len, 0);
}

static int copy_bytes(void *to, char *from, int len) {
  char *winIsDumb = *(char**)to;
  memcpy(winIsDumb, from, len);
  winIsDumb += len;
  *((char**)to) = (void*)winIsDumb;

  return len;
}

static int copy_file(void *to, char *from, int len) {
  int written = fwrite(from, 1, len, (FILE*)to);

  if (written != len) {
    zend_error(E_WARNING, "incorrect byte count.  expected: %d, got %d", len, written);
  }

  return written;
}

static int apply_to_cursor(zval *cursor, apply_copy_func_t apply_copy_func, void *to TSRMLS_DC) {
  int total = 0;
  zval *next;

  MAKE_STD_ZVAL(next);                                                                     
  MONGO_METHOD(MongoCursor, getNext, next, cursor);

  while (Z_TYPE_P(next) != IS_NULL) {
    zval **zdata;

    // check if data field exists.  if it doesn't, we've probably
    // got an error message from the db, so return that
    if (zend_hash_find(HASH_P(next), "data", 5, (void**)&zdata) == FAILURE) {
      if(zend_hash_exists(HASH_P(next), "$err", 5)) {
        return FAILURE;
      }
      continue;
    }

    /* This copies the next chunk -> *to  
     * Due to a talent I have for not reading directions, older versions of the driver
     * store files as raw bytes, not MongoBinData.  So, we'll check for and handle 
     * both cases.
     */
    // raw bytes
    if (Z_TYPE_PP(zdata) == IS_STRING) {
      total += apply_copy_func(to, Z_STRVAL_PP(zdata), Z_STRLEN_PP(zdata));
    }
    // MongoBinData
    else if (Z_TYPE_PP(zdata) == IS_OBJECT &&
             Z_OBJCE_PP(zdata) == mongo_ce_BinData) {
      zval *bin = zend_read_property(mongo_ce_BinData, *zdata, "bin", strlen("bin"), NOISY TSRMLS_CC);
      total += apply_copy_func(to, Z_STRVAL_P(bin), Z_STRLEN_P(bin));
    }
    // if it's not a string or a MongoBinData, give up
    else {
      return FAILURE;
    }

    // get ready for the next iteration
    zval_ptr_dtor(&next);
    MAKE_STD_ZVAL(next);
    MONGO_METHOD(MongoCursor, getNext, next, cursor);
  }
  zval_ptr_dtor(&next);

  // return the number of bytes copied
  return total;
}

static function_entry MongoGridFSFile_methods[] = {
  PHP_ME(MongoGridFSFile, __construct, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFSFile, getFilename, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFSFile, getSize, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFSFile, write, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFSFile, getBytes, NULL, ZEND_ACC_PUBLIC)
  {NULL, NULL, NULL}
};

void mongo_init_MongoGridFSFile(TSRMLS_D) {
  zend_class_entry ce;

  INIT_CLASS_ENTRY(ce, "MongoGridFSFile", MongoGridFSFile_methods);
  mongo_ce_GridFSFile = zend_register_internal_class(&ce TSRMLS_CC);

  zend_declare_property_null(mongo_ce_GridFSFile, "file", strlen("file"), ZEND_ACC_PUBLIC TSRMLS_CC);

  zend_declare_property_null(mongo_ce_GridFSFile, "gridfs", strlen("gridfs"), ZEND_ACC_PROTECTED TSRMLS_CC);
}


PHP_METHOD(MongoGridFSCursor, __construct) {
  zval temp;
  zval *gridfs = 0, *connection = 0, *ns = 0, *query = 0, *fields = 0;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Ozzzz", &gridfs, mongo_ce_GridFS, &connection, &ns, &query, &fields) == FAILURE) {
    return;
  }

  zend_update_property(mongo_ce_GridFSCursor, getThis(), "gridfs", strlen("gridfs"), gridfs TSRMLS_CC);

  MONGO_METHOD4(MongoCursor, __construct, &temp, getThis(), connection, ns, query, fields);
}

PHP_METHOD(MongoGridFSCursor, getNext) {
  MONGO_METHOD(MongoCursor, next, return_value, getThis());
  MONGO_METHOD(MongoGridFSCursor, current, return_value, getThis());
}

PHP_METHOD(MongoGridFSCursor, current) {
  zval temp;
  zval *gridfs;
  mongo_cursor *cursor = (mongo_cursor*)zend_object_store_get_object(getThis() TSRMLS_CC);
  MONGO_CHECK_INITIALIZED(cursor->link, MongoGridFSCursor);

  if (!cursor->current) {
    RETURN_NULL();
  }

  object_init_ex(return_value, mongo_ce_GridFSFile);

  gridfs = zend_read_property(mongo_ce_GridFSCursor, getThis(), "gridfs", strlen("gridfs"), NOISY TSRMLS_CC);

  MONGO_METHOD2(MongoGridFSFile, __construct, &temp, return_value, gridfs, cursor->current);
}

PHP_METHOD(MongoGridFSCursor, key) {
  mongo_cursor *cursor = (mongo_cursor*)zend_object_store_get_object(getThis() TSRMLS_CC);
  MONGO_CHECK_INITIALIZED(cursor->link, MongoGridFSCursor);

  if (!cursor->current) {
    RETURN_NULL();
  }
  zend_hash_find(HASH_P(cursor->current), "filename", strlen("filename")+1, (void**)&return_value_ptr);
  if (!return_value_ptr) {
    RETURN_NULL();
  }
  convert_to_string(*return_value_ptr);
  RETURN_STRING(Z_STRVAL_PP(return_value_ptr), 1);
}


static function_entry MongoGridFSCursor_methods[] = {
  PHP_ME(MongoGridFSCursor, __construct, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFSCursor, getNext, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFSCursor, current, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoGridFSCursor, key, NULL, ZEND_ACC_PUBLIC)
  {NULL, NULL, NULL}
};

void mongo_init_MongoGridFSCursor(TSRMLS_D) {
  zend_class_entry ce;

  INIT_CLASS_ENTRY(ce, "MongoGridFSCursor", MongoGridFSCursor_methods);
  mongo_ce_GridFSCursor = zend_register_internal_class_ex(&ce, mongo_ce_Cursor, "MongoCursor" TSRMLS_CC);

  zend_declare_property_null(mongo_ce_GridFSCursor, "gridfs", strlen("gridfs"), ZEND_ACC_PROTECTED TSRMLS_CC);
}
