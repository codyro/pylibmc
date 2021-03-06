/**
 * _pylibmc: hand-made libmemcached bindings for Python
 *
 * Copyright (c) 2008, Ludvig Ericson
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 * 
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 * 
 *  - Neither the name of the author nor the names of the contributors may be
 *  used to endorse or promote products derived from this software without
 *  specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "_pylibmcmodule.h"
#ifdef USE_ZLIB
#  include <zlib.h>
#  define ZLIB_BUFSZ (1 << 14)
/* only callable while holding the GIL */
#  define _ZLIB_ERR(s, rc) \
  PyErr_Format(PylibMCExc_MemcachedError, "zlib error %d in " s, rc);
#endif


/* {{{ Type methods */
static PylibMC_Client *PylibMC_ClientType_new(PyTypeObject *type,
        PyObject *args, PyObject *kwds) {
    PylibMC_Client *self;

    /* GenericNew calls GenericAlloc (via the indirection type->tp_alloc) which
     * adds GC tracking if flagged for, and also calls PyObject_Init. */
    self = (PylibMC_Client *)PyType_GenericNew(type, args, kwds);

    if (self != NULL) {
        self->mc = memcached_create(NULL);
    }

    return self;
}

static void PylibMC_ClientType_dealloc(PylibMC_Client *self) {
    if (self->mc != NULL) {
        memcached_free(self->mc);
    }

    self->ob_type->tp_free(self);
}
/* }}} */

static int PylibMC_Client_init(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
    PyObject *srvs, *srvs_it, *c_srv;
    unsigned char set_stype = 0, bin = 0, got_server = 0;

    static char *kws[] = { "servers", "binary", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|b", kws, &srvs, &bin)) {
        return -1;
    } else if ((srvs_it = PyObject_GetIter(srvs)) == NULL) {
        return -1;
    }

    memcached_behavior_set(self->mc, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, bin);

    while ((c_srv = PyIter_Next(srvs_it)) != NULL) {
        unsigned char stype;
        char *hostname;
        unsigned short int port;
        memcached_return rc;

        got_server |= 1;
        port = 0;
        if (PyString_Check(c_srv)) {
            memcached_server_st *list;

            list = memcached_servers_parse(PyString_AS_STRING(c_srv));
            if (list == NULL) {
                PyErr_SetString(PylibMCExc_MemcachedError,
                        "memcached_servers_parse returned NULL");
                goto it_error;
            }

            rc = memcached_server_push(self->mc, list);
            free(list);
            if (rc != MEMCACHED_SUCCESS) {
                PylibMC_ErrFromMemcached(self, "memcached_server_push", rc);
                goto it_error;
            }
        } else if (PyArg_ParseTuple(c_srv, "Bs|H", &stype, &hostname, &port)) {
            if (set_stype && set_stype != stype) {
                PyErr_SetString(PyExc_ValueError, "can't mix transport types");
                goto it_error;
            } else {
                set_stype = stype;
                if (stype == PYLIBMC_SERVER_UDP) {
                    memcached_behavior_set(self->mc,
                        MEMCACHED_BEHAVIOR_USE_UDP, 1);
                }
            }

            switch (stype) {
                case PYLIBMC_SERVER_TCP:
                    rc = memcached_server_add(self->mc, hostname, port);
                    break;
                case PYLIBMC_SERVER_UDP:
                    rc = memcached_server_add_udp(self->mc, hostname, port);
                    break;
                case PYLIBMC_SERVER_UNIX:
                    if (port) {
                        PyErr_SetString(PyExc_ValueError,
                                "can't set port on unix sockets");
                        goto it_error;
                    }
                    rc = memcached_server_add_unix_socket(self->mc, hostname);
                    break;
                default:
                    PyErr_Format(PyExc_ValueError, "bad type: %u", stype);
                    goto it_error;
            }
            if (rc != MEMCACHED_SUCCESS) {
                PylibMC_ErrFromMemcached(self, "memcached_server_add_*", rc);
                goto it_error;
            }
        }
        Py_DECREF(c_srv);
        continue;

it_error:
        Py_DECREF(c_srv);
        goto error;
    }

    if (!got_server) {
        PyErr_SetString(PylibMCExc_MemcachedError, "empty server list");
        goto error;
    }

    Py_DECREF(srvs_it);
    return 0;
error:
    Py_DECREF(srvs_it);
    return -1;
}

/* {{{ Compression helpers */
#ifdef USE_ZLIB
static int _PylibMC_Deflate(char* value, size_t value_len,
                    char** result, size_t *result_len) {
    /* todo: failures in here are entirely silent. this should probably
       be fixed */

    z_stream strm;
    *result = NULL;
    *result_len = 0;

    /* Don't ask me about this one. Got it from zlibmodule.c in Python 2.6. */
    size_t out_sz = value_len + value_len / 1000 + 12 + 1;

    if ((*result = malloc(sizeof(char) * out_sz)) == NULL) {
      goto error;
    }

    strm.avail_in = value_len;
    strm.avail_out = out_sz;
    strm.next_in = (Bytef*)value;
    strm.next_out = (Bytef*)*result;

    /* we just pre-allocated all of it up front */
    strm.zalloc = (alloc_func)NULL;
    strm.zfree = (free_func)Z_NULL;

    /* TODO Expose compression level somehow. */
    if (deflateInit((z_streamp)&strm, Z_BEST_SPEED) != Z_OK) {
        goto error;
    }

    if (deflate((z_streamp)&strm, Z_FINISH) != Z_STREAM_END) {
        /* could we be triggering a leak in zlib by not calling
           deflateEnd here? */
        goto error;
    }

    if (deflateEnd((z_streamp)&strm) != Z_OK) {
        goto error;
    }

    if(strm.total_out >= value_len) {
      /* if we didn't actually save anything, don't bother storing it
         compressed */
      goto error;
    }

    /* *result should already be populated since that's the address we
       passed into the z_stream */
    *result_len = strm.total_out;

    return 1;
error:
    /* if any error occurred, we'll just use the original value
       instead of trying to compress it */
    if(*result != NULL) {
      free(*result);
      *result = NULL;
    }
    return 0;
}

static PyObject *_PylibMC_Inflate(char *value, size_t size) {
    int rc;
    char *out;
    PyObject *out_obj;
    Py_ssize_t rvalsz;
    z_stream strm;

    /* Output buffer */
    rvalsz = ZLIB_BUFSZ;
    out_obj = PyString_FromStringAndSize(NULL, rvalsz);
    if (out_obj == NULL) {
        return NULL;
    }
    out = PyString_AS_STRING(out_obj);

    /* Set up zlib stream. */
    strm.avail_in = size;
    strm.avail_out = (uInt)rvalsz;
    strm.next_in = (Byte *)value;
    strm.next_out = (Byte *)out;

    strm.zalloc = (alloc_func)NULL;
    strm.zfree = (free_func)Z_NULL;

    /* TODO Add controlling of windowBits with inflateInit2? */
    if ((rc = inflateInit((z_streamp)&strm)) != Z_OK) {
        _ZLIB_ERR("inflateInit", rc);
        goto error;
    }

    do {
        /* TODO be smarter about when to release the GIL here: for
           very small datasets there's no point but for a very large
           one it might make sense to do during the I/O that obtained
           the value in the first place */
        rc = inflate((z_streamp)&strm, Z_FINISH);

        switch (rc) {
        case Z_STREAM_END:
            break;
        /* When a Z_BUF_ERROR occurs, we should be out of memory.
         * This is also true for Z_OK, hence the fall-through. */
        case Z_BUF_ERROR:
            if (strm.avail_out) {
                _ZLIB_ERR("inflate", rc);
                inflateEnd(&strm);
                goto error;
            }
        /* Fall-through */
        case Z_OK:
            if (_PyString_Resize(&out_obj, (Py_ssize_t)(rvalsz << 1)) < 0) {
                inflateEnd(&strm);
                goto error;
            }
            /* Wind forward */
            out = PyString_AS_STRING(out_obj);
            strm.next_out = (Byte *)(out + rvalsz);
            strm.avail_out = rvalsz;
            rvalsz = rvalsz << 1;
            break;
        default:
            inflateEnd(&strm);
            goto error;
        }
    } while (rc != Z_STREAM_END);

    if ((rc = inflateEnd(&strm)) != Z_OK) {
        _ZLIB_ERR("inflateEnd", rc);
        goto error;
    }

    _PyString_Resize(&out_obj, strm.total_out);
    return out_obj;
error:
    Py_DECREF(out_obj);
    return NULL;
}
#endif
/* }}} */

static PyObject *_PylibMC_parse_memcached_value(char *value, size_t size,
        uint32_t flags) {
    PyObject *retval, *tmp;

#if USE_ZLIB
    PyObject *inflated = NULL;

    /* Decompress value if necessary. */
    if (flags & PYLIBMC_FLAG_ZLIB) {
        inflated = _PylibMC_Inflate(value, size);
        value = PyString_AS_STRING(inflated);
        size = PyString_GET_SIZE(inflated);
    }
#else
    if (flags & PYLIBMC_FLAG_ZLIB) {
        PyErr_SetString(PylibMCExc_MemcachedError,
            "value for key compressed, unable to inflate");
        return NULL;
    }
#endif

    retval = NULL;

    switch (flags & PYLIBMC_FLAG_TYPES) {
        case PYLIBMC_FLAG_PICKLE:
            retval = _PylibMC_Unpickle(value, size);
            break;
        case PYLIBMC_FLAG_INTEGER:
        case PYLIBMC_FLAG_LONG:
            retval = PyInt_FromString(value, NULL, 10);
            break;
        case PYLIBMC_FLAG_BOOL:
            if ((tmp = PyInt_FromString(value, NULL, 10)) == NULL) {
                return NULL;
            }
            retval = PyBool_FromLong(PyInt_AS_LONG(tmp));
            Py_DECREF(tmp);
            break;
        case PYLIBMC_FLAG_NONE:
            retval = PyString_FromStringAndSize(value, (Py_ssize_t)size);
            break;
        default:
            PyErr_Format(PylibMCExc_MemcachedError,
                    "unknown memcached key flags %u", flags);
    }

#if USE_ZLIB
    Py_XDECREF(inflated);
#endif

    return retval;
}

static PyObject *PylibMC_Client_get(PylibMC_Client *self, PyObject *arg) {
    char *mc_val;
    size_t val_size;
    uint32_t flags;
    memcached_return error;

    if (!_PylibMC_CheckKey(arg)) {
        return NULL;
    } else if (!PySequence_Length(arg) ) {
        /* Others do this, so... */
        Py_RETURN_NONE;
    }

    Py_BEGIN_ALLOW_THREADS

    mc_val = memcached_get(self->mc,
            PyString_AS_STRING(arg), PyString_GET_SIZE(arg),
            &val_size, &flags, &error);

    Py_END_ALLOW_THREADS

    if (mc_val != NULL) {
        PyObject *r = _PylibMC_parse_memcached_value(mc_val, val_size, flags);
        free(mc_val);
        return r;
    } else if (error == MEMCACHED_SUCCESS) {
        /* This happens for empty values, and so we fake an empty string. */
        return PyString_FromStringAndSize("", 0);
    } else if (error == MEMCACHED_NOTFOUND) {
        /* Since python-memcache returns None when the key doesn't exist,
         * so shall we. */
        Py_RETURN_NONE;
    }

    return PylibMC_ErrFromMemcached(self, "memcached_get", error);
}

/* {{{ Set commands (set, replace, add, prepend, append) */
static PyObject *_PylibMC_RunSetCommandSingle(PylibMC_Client *self,
        _PylibMC_SetCommand f, char *fname, PyObject *args,
        PyObject *kwds) {
  /* function called by the set/add/etc commands */
  static char *kws[] = { "key", "val", "time", "min_compress_len", NULL };
  PyObject *key;
  PyObject *value;
  unsigned int time = 0; /* this will be turned into a time_t */
  unsigned int min_compress = 0;
  bool success = false;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "SO|II", kws,
                                   &key, &value,
                                   &time, &min_compress)) {
    return NULL;
  }

#ifndef USE_ZLIB
  if (min_compress) {
    PyErr_SetString(PyExc_TypeError, "min_compress_len without zlib");
    return NULL;
  }
#endif

  pylibmc_mset serialized = { NULL, 0,
                              NULL, 0,
                              0, PYLIBMC_FLAG_NONE,
                              NULL, NULL, NULL,
                              false };

  success = _PylibMC_SerializeValue(key, NULL, value, time, &serialized);

  if(!success) goto cleanup;

  success = _PylibMC_RunSetCommand(self, f, fname,
                                   &serialized, 1,
                                   min_compress);

cleanup:
  _PylibMC_FreeMset(&serialized);

  if(PyErr_Occurred() != NULL) {
    return NULL;
  } else if(success) {
    Py_RETURN_TRUE;
  } else {
    Py_RETURN_FALSE;
  }
}

static PyObject *_PylibMC_RunSetCommandMulti(PylibMC_Client* self,
        _PylibMC_SetCommand f, char *fname, PyObject* args,
        PyObject* kwds) {
  /* function called by the set/add/incr/etc commands */
  static char *kws[] = { "keys", "key_prefix", "time", "min_compress_len", NULL };
  PyObject* keys = NULL;
  PyObject* key_prefix = NULL;
  unsigned int time = 0;
  unsigned int min_compress = 0;
  PyObject * retval = NULL;
  size_t idx = 0;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!|O!II", kws,
                                   &PyDict_Type, &keys,
                                   &PyString_Type, &key_prefix,
                                   &time, &min_compress)) {
    return NULL;
  }

#ifndef USE_ZLIB
  if (min_compress) {
    PyErr_SetString(PyExc_TypeError, "min_compress_len without zlib");
    return NULL;
  }
#endif

  PyObject *curr_key, *curr_value;
  size_t nkeys = (size_t)PyDict_Size(keys);

  pylibmc_mset* serialized = PyMem_New(pylibmc_mset, nkeys);
  if(serialized == NULL) {
    goto cleanup;
  }

  for(idx = 0; idx < nkeys; idx++) {
    /* init them all with NULL pointers so that we can reliably detect
       and free the ones that get allocated */
    serialized[idx].key = NULL;
    serialized[idx].key_len = 0;
    serialized[idx].value = NULL;
    serialized[idx].value_len = 0;
    serialized[idx].time = 0;
    serialized[idx].flags = PYLIBMC_FLAG_NONE;
    serialized[idx].key_obj = NULL;
    serialized[idx].prefixed_key_obj = NULL;
    serialized[idx].value_obj = NULL;
    serialized[idx].success = false;
  }

  /* we're pointing into existing Python memory with the 'key' members
     of pylibmc_mset (extracted using PyDict_Next) and during
     _PylibMC_RunSetCommand (which uses those same 'key' params, and
     potentially points into value string objects too), so we don't
     want to go around decrementing any references that risk
     destroying the pointed objects until we're done, especially since
     we're going to release the GIL while we do the I/O that accesses
     that memory. We're assuming that this is safe because Python
     strings are immutable */

  Py_ssize_t pos = 0; /* PyDict_Next's 'pos' isn't an incrementing index */
  idx = 0;
  while(PyDict_Next(keys, &pos, &curr_key, &curr_value)) {
    int success = _PylibMC_SerializeValue(curr_key, key_prefix,
                                          curr_value, time,
                                          &serialized[idx]);
    if(!success || PyErr_Occurred() != NULL) {
      /* exception should already be on the stack */
      goto cleanup;
    }
    idx++;
  }

  if(PyErr_Occurred() != NULL) {
    /* an iteration error of some sort */
    goto cleanup;
  }

  bool allsuccess = _PylibMC_RunSetCommand(self, f, fname, serialized, nkeys, time);

  if(PyErr_Occurred() != NULL) {
    goto cleanup;
  }

  /* We're building the return value for set_multi, which is the list
     of keys that were not successfully set */
  retval = PyList_New(0);
  if(retval == NULL || PyErr_Occurred() != NULL) {
    goto cleanup;
  }
  if(!allsuccess) {
    for(idx = 0; idx < nkeys; idx++) {
      if(!serialized[idx].success) {
        if(PyList_Append(retval, serialized[idx].key_obj) != 0) {
          /* Ugh */

          Py_DECREF(retval);
          retval = NULL;
          goto cleanup;
        }
      }
    }
  }

cleanup:
  if(serialized != NULL) {
    for(pos = 0; pos < nkeys; pos++) {
      _PylibMC_FreeMset(&serialized[pos]);
    }
    PyMem_Free(serialized);
  }

  return retval;
}

static void _PylibMC_FreeMset(pylibmc_mset* mset) {
  mset->key = NULL;
  mset->key_len = 0;
  mset->value = NULL;
  mset->value_len = 0;

  /* if this isn't NULL then we incred it */
  Py_XDECREF(mset->key_obj);
  mset->key_obj = NULL;

  /* if this isn't NULL then we built it */
  Py_XDECREF(mset->prefixed_key_obj);
  mset->prefixed_key_obj = NULL;

  /* this is either a string that we created, or a string that we
     passed to us. in the latter case, we incred it ourselves, so this
     should be safe */
  Py_XDECREF(mset->value_obj);
  mset->value_obj = NULL;
}

static int _PylibMC_SerializeValue(PyObject* key_obj,
                                   PyObject* key_prefix,
                                   PyObject* value_obj,
                                   time_t time,
                                   pylibmc_mset* serialized) {
  /* do the easy bits first */
  serialized->time = time;
  serialized->success = false;
  serialized->flags = PYLIBMC_FLAG_NONE;

  if(!_PylibMC_CheckKey(key_obj)
     || PyString_AsStringAndSize(key_obj, &serialized->key,
                                 &serialized->key_len) == -1) {
    return false;
  }

  /* we need to incr our reference here so that it's guaranteed to
     exist while we release the GIL. Even if we fail after this it
     should be decremeneted by pylib_mset_free */
  serialized->key_obj = key_obj;
  Py_INCREF(key_obj);

  /* make the prefixed key if appropriate */
  if(key_prefix != NULL) {
    if(!_PylibMC_CheckKey(key_prefix)) {
      return false;
    }

    /* we can safely ignore an empty prefix */
    if(PyString_Size(key_prefix) > 0) {
      PyObject* prefixed_key_obj = NULL; /* freed by _PylibMC_FreeMset */
      prefixed_key_obj = PyString_FromFormat("%s%s",
                                             PyString_AS_STRING(key_prefix),
                                             PyString_AS_STRING(key_obj));
      if(prefixed_key_obj == NULL) {
        return false;
      }

      /* check the key and overwrite the C string */
      if(!_PylibMC_CheckKey(prefixed_key_obj)
         || PyString_AsStringAndSize(prefixed_key_obj, &serialized->key,
                                     &serialized->key_len) == -1) {
        Py_DECREF(prefixed_key_obj);
        return false;
      }
      serialized->prefixed_key_obj = prefixed_key_obj;
    }
  }

  /* key/key_size should be copasetic, now onto the value */

  PyObject* store_val = NULL;

  /* first, build store_val, a Python String object, out of the object
     we were passed */
  if (PyString_Check(value_obj)) {
    store_val = value_obj;
    Py_INCREF(store_val); /* because we'll be decring it again in
                             pylibmc_mset_free*/
  } else if (PyBool_Check(value_obj)) {
    serialized->flags |= PYLIBMC_FLAG_BOOL;
    PyObject* tmp = PyNumber_Int(value_obj);
    store_val = PyObject_Str(tmp);
    Py_DECREF(tmp);
  } else if (PyInt_Check(value_obj)) {
    serialized->flags |= PYLIBMC_FLAG_INTEGER;
    PyObject* tmp = PyNumber_Int(value_obj);
    store_val = PyObject_Str(tmp);
    Py_DECREF(tmp);
  } else if (PyLong_Check(value_obj)) {
    serialized->flags |= PYLIBMC_FLAG_LONG;
    PyObject* tmp = PyNumber_Long(value_obj);
    store_val = PyObject_Str(tmp);
    Py_DECREF(tmp);
  } else if(value_obj != NULL) {
    /* we have no idea what it is, so we'll store it pickled */
    Py_INCREF(value_obj);
    serialized->flags |= PYLIBMC_FLAG_PICKLE;
    store_val = _PylibMC_Pickle(value_obj);
    Py_DECREF(value_obj);
  }

  if (store_val == NULL) {
    return false;
  }

  if(PyString_AsStringAndSize(store_val, &serialized->value,
                              &serialized->value_len) == -1) {
    if(serialized->flags == PYLIBMC_FLAG_NONE) {
      /* for some reason we weren't able to extract the value/size
         from a string that we were explicitly passed, that we
         INCREF'd above */
      Py_DECREF(store_val);
    }
    return false;
  }

  /* so now we have a reference to a string that we may have
     created. we need that to keep existing while we release the HIL,
     so we need to hold the reference, but we need to free it up when
     we're done */
  serialized->value_obj = store_val;

  return true;
}

/* {{{ Set commands (set, replace, add, prepend, append) */
static bool _PylibMC_RunSetCommand(PylibMC_Client* self,
                                   _PylibMC_SetCommand f, char *fname,
                                   pylibmc_mset* msets, size_t nkeys,
                                   size_t min_compress) {
    memcached_st* mc = self->mc;
    memcached_return rc = MEMCACHED_SUCCESS;
    int pos;
    bool error = false;
    bool allsuccess = true;

    Py_BEGIN_ALLOW_THREADS

    for(pos=0; pos < nkeys && !error; pos++) {
      pylibmc_mset* mset = &msets[pos];

      char* value = mset->value;
      size_t value_len = mset->value_len;
      uint32_t flags = mset->flags;

#ifdef USE_ZLIB
      char* compressed_value = NULL;
      size_t compressed_len = 0;

      if(min_compress && value_len >= min_compress) {
        _PylibMC_Deflate(value, value_len, &compressed_value, &compressed_len);
      }

      if(compressed_value != NULL) {
        /* will want to change this if this function needs to get back
           at the old *value at some point */
        value = compressed_value;
        value_len = compressed_len;
        flags |= PYLIBMC_FLAG_ZLIB;
      }
#endif

      /* finally go and call the actual libmemcached function */
      if(mset->key_len == 0) {
        /* most other implementations ignore zero-length keys, so
           we'll just do that */
        rc = MEMCACHED_NOTSTORED;
      } else {
        rc = f(mc,
               mset->key, mset->key_len,
               value, value_len,
               mset->time, flags);
      }

#ifdef USE_ZLIB
      if(compressed_value != NULL) {
        free(compressed_value);
      }
#endif

     switch(rc) {
     case MEMCACHED_SUCCESS:
       mset->success = true;
       break;
     case MEMCACHED_FAILURE:
     case MEMCACHED_NO_KEY_PROVIDED:
     case MEMCACHED_BAD_KEY_PROVIDED:
     case MEMCACHED_MEMORY_ALLOCATION_FAILURE:
     case MEMCACHED_DATA_EXISTS:
     case MEMCACHED_NOTSTORED:
       mset->success = false;
       allsuccess = false;
       break;
     default:
       mset->success = false;
       allsuccess = false;
       error = true; /* will break the for loop */
     } /* switch */

  } /* for */

  Py_END_ALLOW_THREADS

  /* we only return the last return value, even for a _multi
     operation, but we do set the success on the mset */
  if(error) {
    PylibMC_ErrFromMemcached(self, fname, rc);
    return false;
  } else {
    return allsuccess;
  }
}

/* These all just call _PylibMC_RunSetCommand with the appropriate
 * arguments.  In other words: bulk. */
static PyObject *PylibMC_Client_set(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
    PyObject* retval = _PylibMC_RunSetCommandSingle(
            self, memcached_set, "memcached_set", args, kwds);
    return retval;
}

static PyObject *PylibMC_Client_replace(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
    return _PylibMC_RunSetCommandSingle(
            self, memcached_replace, "memcached_replace", args, kwds);
}

static PyObject *PylibMC_Client_add(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
    return _PylibMC_RunSetCommandSingle(
            self, memcached_add, "memcached_add", args, kwds);
}

static PyObject *PylibMC_Client_prepend(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
    return _PylibMC_RunSetCommandSingle(
            self, memcached_prepend, "memcached_prepend", args, kwds);
}

static PyObject *PylibMC_Client_append(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
    return _PylibMC_RunSetCommandSingle(
            self, memcached_append, "memcached_append", args, kwds);
}
/* }}} */

static PyObject *PylibMC_Client_delete(PylibMC_Client *self, PyObject *args) {
    PyObject *retval;
    char *key;
    Py_ssize_t key_sz;
    unsigned int time;
    memcached_return rc;

    retval = NULL;
    time = 0;
    if (PyArg_ParseTuple(args, "s#|I", &key, &key_sz, &time)
            && _PylibMC_CheckKeyStringAndSize(key, key_sz)) {
        Py_BEGIN_ALLOW_THREADS
        rc = memcached_delete(self->mc, key, key_sz, time);
        Py_END_ALLOW_THREADS
        switch (rc) {
            case MEMCACHED_SUCCESS:
                Py_RETURN_TRUE;
                break;
            case MEMCACHED_FAILURE:
            case MEMCACHED_NOTFOUND:
            case MEMCACHED_NO_KEY_PROVIDED:
            case MEMCACHED_BAD_KEY_PROVIDED:
                Py_RETURN_FALSE;
                break;
            default:
                return PylibMC_ErrFromMemcached(self, "memcached_delete", rc);
        }
    }

    return NULL;
}

/* {{{ Increment & decrement */
static PyObject *_PylibMC_IncrSingle(PylibMC_Client *self,
                                     _PylibMC_IncrCommand incr_func,
                                     PyObject *args) {
    char *key;
    Py_ssize_t key_len;
    unsigned int delta = 1;

    if (!PyArg_ParseTuple(args, "s#|I", &key, &key_len, &delta)) {
        return NULL;
    } else if (!_PylibMC_CheckKeyStringAndSize(key, key_len)) {
        return NULL;
    }

    pylibmc_incr incr = { key, key_len,
                          incr_func, delta,
                          0 };

    _PylibMC_IncrDecr(self, &incr, 1);

    if(PyErr_Occurred() != NULL) {
      /* exception already on the stack */
      return NULL;
    }

    /* might be NULL, but if that's true then it's the right return value */
    return PyLong_FromUnsignedLong((unsigned long)incr.result);
}

static PyObject *_PylibMC_IncrMulti(PylibMC_Client *self,
                                    _PylibMC_IncrCommand incr_func,
                                    PyObject *args, PyObject *kwds) {
  /* takes an iterable of keys and a single delta (that defaults to 1)
     to be applied to all of them. Consider the return value and
     exception behaviour to be undocumented: for now it returns None
     and throws an exception on an error incrementing any key */
  PyObject* keys = NULL;
  PyObject* key_prefix = NULL;
  PyObject* prefixed_keys = NULL;
  PyObject* retval = NULL;
  PyObject* iterator = NULL;
  unsigned int delta = 1;

  static char *kws[] = { "keys", "key_prefix", "delta", NULL };

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|SI", kws,
                                   &keys, &key_prefix, &delta)) {
    return NULL;
  }

  size_t nkeys = (size_t)PySequence_Size(keys);
  if(nkeys == -1)
    return NULL;

  if(key_prefix == NULL || PyString_Size(key_prefix) < 1) {
    /* if it's 0-length, we can safely pretend it doesn't exist */
    key_prefix = NULL;
  }
  if(key_prefix != NULL) {
    if(!_PylibMC_CheckKey(key_prefix)) {
      return NULL;
    }

    prefixed_keys = PyList_New(nkeys);
    if(prefixed_keys == NULL) {
      return NULL;
    }
  }

  pylibmc_incr* incrs = PyMem_New(pylibmc_incr, nkeys);
  if(incrs == NULL) {
    goto cleanup;
  }

  iterator = PyObject_GetIter(keys);
  if(iterator == NULL) {
    goto cleanup;
  }

  PyObject* key = NULL;
  size_t idx = 0;

  /* build our list of keys, prefixed as appropriate, and turn that
     into a list of pylibmc_incr objects that can be incred in one
     go. We're not going to own references to the prefixed keys: so
     that we can free them all at once, we'll give ownership to a list
     of them (prefixed_keys) which we'll DECR once at the end */
  while((key = PyIter_Next(iterator)) != NULL) {
    if(!_PylibMC_CheckKey(key)) goto loopcleanup;

    if(key_prefix != NULL) {

      PyObject* newkey = NULL;

      newkey = PyString_FromFormat("%s%s",
                                   PyString_AS_STRING(key_prefix),
                                   PyString_AS_STRING(key));
      if(!_PylibMC_CheckKey(newkey)) {
        Py_XDECREF(newkey);
        goto loopcleanup;
      }

      /* steals our reference */
      if(PyList_SetItem(prefixed_keys, idx, newkey) == -1) {
        /* it wasn't stolen before the error */
        Py_DECREF(newkey);
        goto loopcleanup;
      }

      /* the list stole our reference to it, and we're going to rely
         on that list to maintain it while we release the GIL, but
         since we DECREF the key in loopcleanup we need to INCREF it
         here */
      Py_DECREF(key);
      Py_INCREF(newkey);
      key = newkey;
    }

    if(PyString_AsStringAndSize(key, &incrs[idx].key, &incrs[idx].key_len) == -1)
      goto loopcleanup;

    incrs[idx].delta = delta;
    incrs[idx].incr_func = incr_func;
    incrs[idx].result = 0; /* after incring we have no way of knowing
                              whether the real result is 0 or if the
                              incr wasn't successful (or if noreply is
                              set), but since we're not actually
                              returning the result that's okay for
                              now */

loopcleanup:
    Py_DECREF(key);

    if(PyErr_Occurred())
      break;

    idx++;
  }

  /* iteration error */
  if (PyErr_Occurred()) goto cleanup;

  _PylibMC_IncrDecr(self, incrs, nkeys);

  /* if that failed, there's an exception on the stack */
  if(PyErr_Occurred()) goto cleanup;

  retval = Py_None;
  Py_INCREF(retval);

cleanup:
  if(incrs != NULL) {
    PyMem_Free(incrs);
  }

  Py_XDECREF(prefixed_keys);
  Py_XDECREF(iterator);

  return retval;
}



static PyObject *PylibMC_Client_incr(PylibMC_Client *self, PyObject *args) {
  return _PylibMC_IncrSingle(self, memcached_increment, args);
}

static PyObject *PylibMC_Client_decr(PylibMC_Client *self, PyObject *args) {
  return _PylibMC_IncrSingle(self, memcached_decrement, args);
}

static PyObject *PylibMC_Client_incr_multi(PylibMC_Client *self, PyObject *args,
                                           PyObject *kwds) {
  return _PylibMC_IncrMulti(self, memcached_increment, args, kwds);
}

static bool _PylibMC_IncrDecr(PylibMC_Client *self, pylibmc_incr *incrs,
        size_t nkeys) {

  bool error = false;
  memcached_return rc = MEMCACHED_SUCCESS;
  _PylibMC_IncrCommand f = NULL;
  size_t i;

  Py_BEGIN_ALLOW_THREADS
  for(i = 0; i < nkeys && !error; i++) {
    pylibmc_incr *incr = &incrs[i];
    uint64_t result = 0;
    f = incr->incr_func;
    rc = f(self->mc, incr->key, incr->key_len, incr->delta, &result);
    if (rc == MEMCACHED_SUCCESS) {
      incr->result = result;
    } else {
      error = true;
    }
  }
  Py_END_ALLOW_THREADS

  if(error) {
      char *fname = (f == memcached_decrement) ? "memcached_decrement"
                                               : "memcached_increment";
      PylibMC_ErrFromMemcached(self, fname, rc);
      return false;
  } else {
    return true;
  }
}
/* }}} */

memcached_return pylibmc_memcached_fetch_multi(memcached_st* mc,
                                               char** keys,
                                               size_t nkeys,
                                               size_t* key_lens,
                                               pylibmc_mget_result* results,
                                               size_t* nresults,
                                               char** err_func) {

  /* the part of PylibMC_Client_get_multi that does the blocking I/O
     and can be called while not holding the GIL. Builds an
     intermediate result set into 'results' that is turned into a
     PyDict before being returned to the caller */

  memcached_return rc;
  char curr_key[MEMCACHED_MAX_KEY];
  size_t curr_key_len = 0;
  char* curr_value = NULL;
  size_t curr_value_len = 0;
  uint32_t curr_flags = 0;

  *nresults = 0;

  rc = memcached_mget(mc, (const char **)keys, key_lens, nkeys);

  if(rc != MEMCACHED_SUCCESS) {
    *err_func = "memcached_mget";
    return rc;
  }

  while((curr_value = memcached_fetch(mc, curr_key, &curr_key_len,
                                      &curr_value_len, &curr_flags, &rc))
        != NULL) {
    if(curr_value == NULL && rc == MEMCACHED_END) {
      return MEMCACHED_SUCCESS;
    } else if(rc == MEMCACHED_BAD_KEY_PROVIDED
           || rc == MEMCACHED_NO_KEY_PROVIDED) {
      /* just skip this key */
    } else if (rc != MEMCACHED_SUCCESS) {
      *err_func = "memcached_fetch";
      return rc;
    } else {
      pylibmc_mget_result r = {"",
                               curr_key_len,
                               curr_value,
                               curr_value_len,
                               curr_flags};
      assert(curr_key_len <= MEMCACHED_MAX_KEY);
      bcopy(curr_key, r.key, curr_key_len);
      results[*nresults] = r;
      *nresults += 1;
    }
  }

  return MEMCACHED_SUCCESS;
}


static PyObject *PylibMC_Client_get_multi(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
    PyObject *key_seq, **key_objs, *retval = NULL;
    char **keys, *prefix = NULL;
    pylibmc_mget_result* results = NULL;
    Py_ssize_t prefix_len = 0;
    Py_ssize_t i;
    PyObject *key_it, *ckey;
    size_t *key_lens;
    size_t nkeys, nresults = 0;
    memcached_return rc;

    char** err_func = NULL;

    static char *kws[] = { "keys", "key_prefix", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|s#", kws,
            &key_seq, &prefix, &prefix_len)) {
        return NULL;
    }

    if ((nkeys = (size_t)PySequence_Length(key_seq)) == -1) {
        return NULL;
    }

    /* this is over-allocating in the majority of cases */
    results = PyMem_New(pylibmc_mget_result, nkeys);

    /* Populate keys and key_lens. */
    keys = PyMem_New(char *, nkeys);
    key_lens = PyMem_New(size_t, nkeys);
    key_objs = PyMem_New(PyObject *, nkeys);
    if (results == NULL || keys == NULL || key_lens == NULL
     || key_objs == NULL) {
        PyMem_Free(results);
        PyMem_Free(keys);
        PyMem_Free(key_lens);
        PyMem_Free(key_objs);
        return PyErr_NoMemory();
    }

    /* Clear potential previous exception, because we explicitly check for
     * exceptions as a loop predicate. */
    PyErr_Clear();

    /* Iterate through all keys and set lengths etc. */
    i = 0;
    key_it = PyObject_GetIter(key_seq);
    while (!PyErr_Occurred()
            && i < nkeys
            && (ckey = PyIter_Next(key_it)) != NULL) {
        PyObject *rkey;

        if (!_PylibMC_CheckKey(ckey)) {
            break;
        } else {
            key_lens[i] = (size_t)(PyString_GET_SIZE(ckey) + prefix_len);
            if (prefix != NULL) {
                rkey = PyString_FromFormat("%s%s",
                        prefix, PyString_AS_STRING(ckey));
                Py_DECREF(ckey);
            } else {
                rkey = ckey;
            }
            keys[i] = PyString_AS_STRING(rkey);
            key_objs[i++] = rkey;
        }
    }
    Py_XDECREF(key_it);

    if (nkeys != 0 && i != nkeys) {
        /* There were keys given, but some keys didn't pass validation. */
        nkeys = 0;
        goto cleanup;
    } else if (nkeys == 0) {
        retval = PyDict_New();
        goto earlybird;
    } else if (PyErr_Occurred()) {
        nkeys--;
        goto cleanup;
    }

    /* TODO Make an iterator interface for getting each key separately.
     *
     * This would help large retrievals, as a single dictionary containing all
     * the data at once isn't needed. (Should probably look into if it's even
     * worth it.)
     */
    Py_BEGIN_ALLOW_THREADS
    rc = pylibmc_memcached_fetch_multi(self->mc,
                                       keys, nkeys, key_lens,
                                       results,
                                       &nresults,
                                       err_func);
    Py_END_ALLOW_THREADS

    if(rc != MEMCACHED_SUCCESS) {
      PylibMC_ErrFromMemcached(self, *err_func, rc);
      goto cleanup;
    }

    retval = PyDict_New();

    for(i = 0; i<nresults; i++) {
      PyObject *val;

      /* This is safe because libmemcached's max key length
       * includes space for a NUL-byte. */
      results[i].key[results[i].key_len] = 0;
      val = _PylibMC_parse_memcached_value(results[i].value,
                                           results[i].value_len,
                                           results[i].flags);
      if (val == NULL) {
        /* PylibMC_parse_memcached_value raises the exception on its
           own */
        goto cleanup;
      }
      PyDict_SetItemString(retval, results[i].key + prefix_len,
                           val);
      Py_DECREF(val);

      if(PyErr_Occurred()) {
        /* only PyDict_SetItemString can incur this one */
        goto cleanup;
      }
    }

earlybird:
    PyMem_Free(key_lens);
    PyMem_Free(keys);
    for (i = 0; i < nkeys; i++) {
        Py_DECREF(key_objs[i]);
    }
    if(results != NULL){
        for (i = 0; i < nresults; i++) {
            /* libmemcached mallocs, so we need to free its memory in
               the same way */
            free(results[i].value);
        }
        PyMem_Free(results);
    }
    PyMem_Free(key_objs);

    /* Not INCREFing because the only two outcomes are NULL and a new dict.
     * We're the owner of that dict already, so. */
    return retval;

cleanup:
    Py_XDECREF(retval);
    PyMem_Free(key_lens);
    PyMem_Free(keys);
    for (i = 0; i < nkeys; i++)
        Py_DECREF(key_objs[i]);
    if(results != NULL){
        for (i = 0; i < nresults; i++) {
            free(results[i].value);
        }
        PyMem_Free(results);
    }
    PyMem_Free(key_objs);
    return NULL;
}

/**
 * Run func over every item in value, building arguments of:
 *     *(item + extra_args)
 */
static PyObject *_PylibMC_DoMulti(PyObject *values, PyObject *func,
        PyObject *prefix, PyObject *extra_args) {
    /* TODO: acquire/release the GIL only once per DoMulti rather than
       once per action; fortunately this is only used for
       delete_multi, which isn't used very often */

    PyObject *retval = PyList_New(0);
    PyObject *iter = NULL;
    PyObject *item = NULL;
    int is_mapping = PyMapping_Check(values);

    if (retval == NULL)
        goto error;

    if ((iter = PyObject_GetIter(values)) == NULL)
        goto error;

    while ((item = PyIter_Next(iter)) != NULL) {
        PyObject *args_f = NULL;
        PyObject *args = NULL;
        PyObject *key = NULL;
        PyObject *ro = NULL;

        /* Calculate key. */
        if (prefix == NULL || prefix == Py_None) {
            /* We now have two owned references to item. */
            key = item;
            Py_INCREF(key);
        } else {
            key = PySequence_Concat(prefix, item);
        }
        if (key == NULL || !_PylibMC_CheckKey(key))
            goto iter_error;

        /* Calculate args. */
        if (is_mapping) {
            PyObject *value;
            char *key_str = PyString_AS_STRING(item);

            if ((value = PyMapping_GetItemString(values, key_str)) == NULL)
                goto iter_error;

            args = PyTuple_Pack(2, key, value);
            Py_DECREF(value);
        } else {
            args = PyTuple_Pack(1, key);
        }
        if (args == NULL)
            goto iter_error;

        /* Calculate full argument tuple. */
        if (extra_args == NULL) {
            Py_INCREF(args);
            args_f = args;
        } else {
            if ((args_f = PySequence_Concat(args, extra_args)) == NULL)
                goto iter_error;
        }

        /* Call stuff. */
        ro = PyObject_CallObject(func, args_f);
        /* This is actually safe even if True got deleted because we're
         * only comparing addresses. */
        Py_XDECREF(ro);
        if (ro == NULL) {
            goto iter_error;
        } else if (ro != Py_True) {
            if (PyList_Append(retval, item) != 0)
                goto iter_error;
        }
        Py_DECREF(args_f);
        Py_DECREF(args);
        Py_DECREF(key);
        Py_DECREF(item);
        continue;
iter_error:
        Py_XDECREF(args_f);
        Py_XDECREF(args);
        Py_XDECREF(key);
        Py_DECREF(item);
        goto error;
    }
    Py_DECREF(iter);

    return retval;
error:
    Py_XDECREF(retval);
    Py_XDECREF(iter);
    return NULL;
}

static PyObject *PylibMC_Client_set_multi(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
  return _PylibMC_RunSetCommandMulti(self, memcached_set, "memcached_set_multi",
                                     args, kwds);
}

static PyObject *PylibMC_Client_add_multi(PylibMC_Client *self, PyObject *args,
        PyObject *kwds) {
  return _PylibMC_RunSetCommandMulti(self, memcached_add, "memcached_add_multi",
                                     args, kwds);
}

static PyObject *PylibMC_Client_delete_multi(PylibMC_Client *self,
        PyObject *args, PyObject *kwds) {
    PyObject *prefix = NULL;
    PyObject *time = NULL;
    PyObject *delete;
    PyObject *keys;
    PyObject *call_args;
    PyObject *retval;

    static char *kws[] = { "keys", "time", "key_prefix", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O!S", kws,
                &keys, &PyInt_Type, &time, &prefix))
        return NULL;

    /**
     * Because of how DoMulti works, we have to prohibit the use of mappings
     * here. Otherwise, the values of the mapping will be the second argument
     * to the delete function; the time argument will then become a third
     * argument, which delete doesn't take.
     *
     * So a mapping to DoMulti would produce calls like:
     *   DoMulti({"a": 1, "b": 2}, time=3)
     *      delete("a", 1, 3)
     *      delete("b", 2, 3)
     */
    if (PyMapping_Check(keys)) {
        PyErr_SetString(PyExc_TypeError,
            "keys must be a sequence, not a mapping");
        return NULL;
    }

    if ((delete = PyObject_GetAttrString((PyObject *)self, "delete")) == NULL)
        return NULL;

    if (time == NULL) {
        retval = _PylibMC_DoMulti(keys, delete, prefix, NULL);
    } else {
        if ((call_args = PyTuple_Pack(1, time)) == NULL)
            goto error;
        retval = _PylibMC_DoMulti(keys, delete, prefix, call_args);
        Py_DECREF(call_args);
    }
    Py_DECREF(delete);

    if (retval == NULL)
        return NULL;

    if (PyList_Size(retval) == 0) {
        Py_DECREF(retval);
        retval = Py_True;
    } else {
        Py_DECREF(retval);
        retval = Py_False;
    }
    Py_INCREF(retval);

    return retval;
error:
    Py_XDECREF(delete);
    return NULL;
}

static PyObject *PylibMC_Client_get_behaviors(PylibMC_Client *self) {
    PyObject *retval = PyDict_New();
    PylibMC_Behavior *b;

    for (b = PylibMC_behaviors; b->name != NULL; b++) {
        uint64_t bval;
        PyObject *x;

        bval = memcached_behavior_get(self->mc, b->flag);
        x = PyInt_FromLong((long)bval);
        if (x == NULL || PyDict_SetItemString(retval, b->name, x) == -1) {
            goto error;
        }

        Py_DECREF(x);
    }

    return retval;
error:
    Py_XDECREF(retval);
    return NULL;
}

static PyObject *PylibMC_Client_set_behaviors(PylibMC_Client *self,
        PyObject *behaviors) {
    PylibMC_Behavior *b;

    for (b = PylibMC_behaviors; b->name != NULL; b++) {
        PyObject *v;
        memcached_return r;

        if (!PyMapping_HasKeyString(behaviors, b->name)) {
            continue;
        } else if ((v = PyMapping_GetItemString(behaviors, b->name)) == NULL) {
            goto error;
        } else if (!PyInt_Check(v)) {
            PyErr_Format(PyExc_TypeError, "behavior %s must be int", b->name);
            goto error;
        }

        r = memcached_behavior_set(self->mc, b->flag, (uint64_t)PyInt_AS_LONG(v));
        Py_DECREF(v);
        if (r != MEMCACHED_SUCCESS) {
            PyErr_Format(PylibMCExc_MemcachedError,
                         "memcached_behavior_set returned %d", r);
            goto error;
        }
    }

    Py_RETURN_NONE;
error:
    return NULL;
}

static PyObject *PylibMC_Client_flush_all(PylibMC_Client *self,
        PyObject *args, PyObject *kwds) {
    memcached_return rc;
    time_t expire = 0;
    PyObject *time = NULL;

    static char *kws[] = { "time", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!", kws,
                                     &PyInt_Type, &time))
        return NULL;

    if (time != NULL)
        expire = PyInt_AS_LONG(time);

    expire = (expire > 0) ? expire : 0;

    Py_BEGIN_ALLOW_THREADS
    rc = memcached_flush(self->mc, expire);
    Py_END_ALLOW_THREADS
    if (rc != MEMCACHED_SUCCESS)
        return PylibMC_ErrFromMemcached(self, "flush_all", rc);

    Py_RETURN_TRUE;
}

static PyObject *PylibMC_Client_disconnect_all(PylibMC_Client *self) {
    Py_BEGIN_ALLOW_THREADS
    memcached_quit(self->mc);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject *PylibMC_Client_clone(PylibMC_Client *self) {
    /* Essentially this is a reimplementation of the allocator, only it uses a
     * cloned memcached_st for mc. */
    PylibMC_Client *clone;

    clone = (PylibMC_Client *)PyType_GenericNew(self->ob_type, NULL, NULL);
    if (clone == NULL) {
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    clone->mc = memcached_clone(NULL, self->mc);
    Py_END_ALLOW_THREADS
    return (PyObject *)clone;
}
/* }}} */

static PyObject *PylibMC_ErrFromMemcached(PylibMC_Client *self, const char *what,
        memcached_return error) {
    if (error == MEMCACHED_ERRNO) {
        PyErr_Format(PylibMCExc_MemcachedError,
                "system error %d from %s: %s", errno, what, strerror(errno));
    /* The key exists, but it has no value */
    } else if (error == MEMCACHED_SUCCESS) {
        PyErr_Format(PyExc_RuntimeError, "error == 0? %s:%d",
                     __FILE__, __LINE__);
    } else { 
        PylibMC_McErr *err;
        PyObject *exc = (PyObject *)PylibMCExc_MemcachedError;

        for (err = PylibMCExc_mc_errs; err->name != NULL; err++) {
            if (err->rc == error) {
                exc = err->exc;
                break;
            }
        }

        PyErr_Format(exc, "error %d from %s: %s", error, what,
                     memcached_strerror(self->mc, error));
    }
    return NULL;
}

/* {{{ Pickling */
static PyObject *_PylibMC_GetPickles(const char *attname) {
    PyObject *pickle, *pickle_attr;

    pickle_attr = NULL;
    /* Import cPickle or pickle. */
    pickle = PyImport_ImportModule("cPickle");
    if (pickle == NULL) {
        PyErr_Clear();
        pickle = PyImport_ImportModule("pickle");
    }

    /* Find attribute and return it. */
    if (pickle != NULL) {
        pickle_attr = PyObject_GetAttrString(pickle, attname);
        Py_DECREF(pickle);
    }

    return pickle_attr;
}

static PyObject *_PylibMC_Unpickle(const char *buff, size_t size) {
    PyObject *pickle_load;
    PyObject *retval = NULL;
    
    retval = NULL;
    pickle_load = _PylibMC_GetPickles("loads");
    if (pickle_load != NULL) {
        retval = PyObject_CallFunction(pickle_load, "s#", buff, size);
        Py_DECREF(pickle_load);
    }

    return retval;
}

static PyObject *_PylibMC_Pickle(PyObject *val) {
    PyObject *pickle_dump;
    PyObject *retval = NULL;

    pickle_dump = _PylibMC_GetPickles("dumps");
    if (pickle_dump != NULL) {
        retval = PyObject_CallFunction(pickle_dump, "Oi", val, -1);
        Py_DECREF(pickle_dump);
    }

    return retval;
}
/* }}} */

static int _PylibMC_CheckKey(PyObject *key) {
    if (key == NULL) {
        PyErr_SetString(PyExc_ValueError, "key must be given");
        return 0;
    } else if (!PyString_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "key must be an instance of str");
        return 0;
    }

    return _PylibMC_CheckKeyStringAndSize(
            PyString_AS_STRING(key), PyString_GET_SIZE(key));
}

static int _PylibMC_CheckKeyStringAndSize(char *key, Py_ssize_t size) {
    if (size > MEMCACHED_MAX_KEY) {
        PyErr_Format(PyExc_ValueError, "key too long, max is %d",
                MEMCACHED_MAX_KEY);
        return 0;
    }
    /* TODO Check key contents here. */

    return key != NULL;
}

static PyMethodDef PylibMC_functions[] = {
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC init_pylibmc(void) {
    PyObject *module, *exc_objs;
    PylibMC_Behavior *b;
    PylibMC_McErr *err;
    int libmemcached_version_minor;
    char name[128];

    /* Check minimum requirement of libmemcached version */
    libmemcached_version_minor = \
        atoi(strchr(LIBMEMCACHED_VERSION_STRING, '.') + 1);
    if (libmemcached_version_minor < 32) {
        PyErr_Format(PyExc_RuntimeError,
            "pylibmc requires >= libmemcached 0.32, was compiled with %s",
            LIBMEMCACHED_VERSION_STRING);
        return;
    }

    if (PyType_Ready(&PylibMC_ClientType) < 0) {
        return;
    }

    module = Py_InitModule3("_pylibmc", PylibMC_functions,
            "Hand-made wrapper for libmemcached.\n\
\n\
You ought to look at python-memcached's documentation for now, seeing\n\
as this module is more or less a drop-in replacement, the difference\n\
being in how you connect. Therefore that's documented here::\n\
\n\
    c = _pylibmc.client([(_pylibmc.server_type_tcp, 'localhost', 11211)])\n\
\n\
As you see, a list of three-tuples of (type, host, port) is used. If \n\
type is `server_type_unix`, no port should be given. A simpler form \n\
can be used as well::\n\
\n\
   c = _pylibmc.client('localhost')\n\
\n\
See libmemcached's memcached_servers_parse for more info on that. I'm told \n\
you can use UNIX domain sockets by specifying paths, and multiple servers \n\
by using comma-separation. Good luck with that.\n\
\n\
Oh, and: plankton.\n");
    if (module == NULL) {
        return;
    }

    PyModule_AddStringConstant(module, "__version__", PYLIBMC_VERSION);

#ifdef USE_ZLIB
    Py_INCREF(Py_True);
    PyModule_AddObject(module, "support_compression", Py_True);
#else
    Py_INCREF(Py_False);
    PyModule_AddObject(module, "support_compression", Py_False);
#endif

    PylibMCExc_MemcachedError = PyErr_NewException(
            "_pylibmc.MemcachedError", NULL, NULL);
    PyModule_AddObject(module, "MemcachedError",
                       (PyObject *)PylibMCExc_MemcachedError);

    exc_objs = PyList_New(0);
    PyList_Append(exc_objs,
        Py_BuildValue("sO", "Error", (PyObject *)PylibMCExc_MemcachedError));

    for (err = PylibMCExc_mc_errs; err->name != NULL; err++) {
        char excnam[64];
        strncpy(excnam, "_pylibmc.", 64);
        strncat(excnam, err->name, 64);
        err->exc = PyErr_NewException(excnam, PylibMCExc_MemcachedError, NULL);
        PyModule_AddObject(module, err->name, (PyObject *)err->exc);
        PyList_Append(exc_objs,
            Py_BuildValue("sO", err->name, (PyObject *)err->exc));
    }

    PyModule_AddObject(module, "exceptions", exc_objs);

    Py_INCREF(&PylibMC_ClientType);
    PyModule_AddObject(module, "client", (PyObject *)&PylibMC_ClientType);

    PyModule_AddIntConstant(module, "server_type_tcp", PYLIBMC_SERVER_TCP);
    PyModule_AddIntConstant(module, "server_type_udp", PYLIBMC_SERVER_UDP);
    PyModule_AddIntConstant(module, "server_type_unix", PYLIBMC_SERVER_UNIX);

    /* Add hasher and distribution constants. */
    for (b = PylibMC_hashers; b->name != NULL; b++) {
        sprintf(name, "hash_%s", b->name);
        PyModule_AddIntConstant(module, name, b->flag);
    }
    for (b = PylibMC_distributions; b->name != NULL; b++) {
        sprintf(name, "distribution_%s", b->name);
        PyModule_AddIntConstant(module, name, b->flag);
    }

    PyModule_AddStringConstant(module,
            "libmemcached_version", LIBMEMCACHED_VERSION_STRING);
}
