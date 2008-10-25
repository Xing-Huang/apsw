/*
  Utility macros and functions

  Copyright (C) 2004-2008 Roger Binns <rogerb@rogerbinns.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any
  damages arising from the use of this software.
 
  Permission is granted to anyone to use this software for any
  purpose, including commercial applications, and to alter it and
  redistribute it freely, subject to the following restrictions:
 
  1. The origin of this software must not be misrepresented; you must
     not claim that you wrote the original software. If you use this
     software in a product, an acknowledgment in the product
     documentation would be appreciated but is not required.

  2. Altered source versions must be plainly marked as such, and must
     not be misrepresented as being the original software.

  3. This notice may not be removed or altered from any source
     distribution.
 
*/

#ifdef __GNUC__
#define APSW_ARGUNUSED __attribute__ ((unused))
#else
#define APSW_ARGUNUSED
#endif



/* used to decide if we will use int or long long, sqlite limit tests due to it not being 64 bit correct */
#define APSW_INT32_MIN (-2147483647-1)
#define APSW_INT32_MAX 2147483647


/* 
   The default Python PyErr_WriteUnraiseable is almost useless.  It
   only prints the str() of the exception and the str() of the object
   passed in.  This gives the developer no clue whatsoever where in
   the code it is happening.  It also does funky things to the passed
   in object which can cause the destructor to fire twice.
   Consequently we use our version here.  It makes the traceback
   complete, and then tries the following, going to the next if
   the hook isn't found or returns an error:

   * excepthook of hookobject (if not NULL)
   * excepthook of sys module
   * PyErr_Display

   If any return an error then then the next one is tried.  When we
   return, any error will be cleared.
*/
static void 
apsw_write_unraiseable(PyObject *hookobject)
{
  PyObject *err_type=NULL, *err_value=NULL, *err_traceback=NULL;
  PyObject *excepthook=NULL;
  PyObject *result=NULL;
  PyFrameObject *frame=NULL;

  /* fill in the rest of the traceback */
  frame = PyThreadState_GET()->frame;
  while(frame)
    {
      PyTraceBack_Here(frame);
      frame=frame->f_back;
    }
  
  /* Get the exception details */
  PyErr_Fetch(&err_type, &err_value, &err_traceback);
  PyErr_NormalizeException(&err_type, &err_value, &err_traceback);

  if(hookobject)
    {
      excepthook=PyObject_GetAttrString(hookobject, "excepthook");
      PyErr_Clear();
      if(excepthook)
        {
          result=PyEval_CallFunction(excepthook, "(OOO)", err_type?err_type:Py_None, err_value?err_value:Py_None, err_traceback?err_traceback:Py_None);
          if(result)
            goto finally;
        }
      Py_XDECREF(excepthook);
    }

  excepthook=PySys_GetObject("excepthook");
  if(excepthook)
    {
      Py_INCREF(excepthook); /* borrowed reference from PySys_GetObject so we increment */
      PyErr_Clear();
      result=PyEval_CallFunction(excepthook, "(OOO)", err_type?err_type:Py_None, err_value?err_value:Py_None, err_traceback?err_traceback:Py_None);
      if(result) 
        goto finally;
    }

  /* remove any error from callback failure */
  PyErr_Clear();
  PyErr_Display(err_type, err_value, err_traceback);

  finally:
  Py_XDECREF(excepthook);
  Py_XDECREF(result);
  Py_XDECREF(err_traceback);
  Py_XDECREF(err_value);
  Py_XDECREF(err_type);
  PyErr_Clear(); /* being paranoid - make sure no errors on return */
}



/* 
   Python's handling of Unicode is horrible.  It can use 2 or 4 byte
   unicode chars and the conversion routines like to put out BOMs
   which makes life even harder.  These macros are used in pairs to do
   the right form of conversion and tell us whether to use the plain
   or -16 version of the SQLite function that is about to be called.
*/

#if Py_UNICODE_SIZE==2
#define UNIDATABEGIN(obj) \
{                                                        \
  size_t strbytes=2*PyUnicode_GET_SIZE(obj);             \
  const void *strdata=PyUnicode_AS_DATA(obj);            

#define UNIDATAEND(obj)                                  \
}

#define USE16(x) x##16

#else  /* Py_UNICODE_SIZE!=2 */

#define UNIDATABEGIN(obj) \
{                                                        \
  Py_ssize_t strbytes=0;				 \
  const char *strdata=NULL;                              \
  PyObject *_utf8=NULL;                                  \
  _utf8=PyUnicode_AsUTF8String(obj);                     \
  if(_utf8)                                              \
    {                                                    \
      strbytes=PyBytes_GET_SIZE(_utf8);                  \
      strdata=PyBytes_AS_STRING(_utf8);                  \
    } 

#define UNIDATAEND(obj)                                  \
  Py_XDECREF(_utf8);                                     \
}

#define USE16(x) x

#endif /* Py_UNICODE_SIZE */

/* Converts sqlite3_value to PyObject.  Returns a new reference. */
static PyObject *
convert_value_to_pyobject(sqlite3_value *value)
{
  int coltype=sqlite3_value_type(value);

  APSW_FAULT_INJECT(UnknownValueType,,coltype=123456);

  switch(coltype)
    {
    case SQLITE_INTEGER:
      {
        sqlite3_int64 val=sqlite3_value_int64(value);
#if PY_MAJOR_VERSION<3
        if (val>=APSW_INT32_MIN && val<=APSW_INT32_MAX)
          return PyInt_FromLong((long)val);
#endif
        return PyLong_FromLongLong(val);
      }

    case SQLITE_FLOAT:
      return PyFloat_FromDouble(sqlite3_value_double(value));
      
    case SQLITE_TEXT:
      return convertutf8stringsize((const char*)sqlite3_value_text(value), sqlite3_value_bytes(value));

    case SQLITE_NULL:
      Py_RETURN_NONE;

    case SQLITE_BLOB:
      return converttobytes(sqlite3_value_blob(value), sqlite3_value_bytes(value));

    default:
      PyErr_Format(APSWException, "Unknown sqlite column type %d!", coltype);
      return NULL;
    }
  /* can't get here */
  assert(0);
  return NULL;
}

/* Converts column to PyObject.  Returns a new reference. Almost identical to above 
   but we cannot just use sqlite3_column_value and then call the above function as 
   SQLite doesn't allow that ("unprotected values") */
static PyObject *
convert_column_to_pyobject(sqlite3_stmt *stmt, int col)
{
  int coltype=sqlite3_column_type(stmt, col);

  APSW_FAULT_INJECT(UnknownColumnType,,coltype=12348);

  switch(coltype)
    {
    case SQLITE_INTEGER:
      {
        sqlite3_int64 val=sqlite3_column_int64(stmt, col);
#if PY_MAJOR_VERSION<3
        if (val>=APSW_INT32_MIN && val<=APSW_INT32_MAX)
          return PyInt_FromLong((long)val);
#endif
        return PyLong_FromLongLong(val);
      }

    case SQLITE_FLOAT:
      return PyFloat_FromDouble(sqlite3_column_double(stmt, col));
      
    case SQLITE_TEXT:
      return convertutf8stringsize((const char*)sqlite3_column_text(stmt, col), sqlite3_column_bytes(stmt, col));

    case SQLITE_NULL:
      Py_RETURN_NONE;

    case SQLITE_BLOB:
      return converttobytes(sqlite3_column_blob(stmt, col), sqlite3_column_bytes(stmt, col));

    default:
      PyErr_Format(APSWException, "Unknown sqlite column type %d!", coltype);
      return NULL;
    }
  /* can't get here */
  assert(0);
  return NULL;
}


/* Some macros used for frequent operations */

/* used by Connection and Cursor */
#define CHECK_USE(e)                                                \
  { if(self->inuse)                                                                                 \
      {    /* raise exception if we aren't already in one */                                                                         \
           if (!PyErr_Occurred())                                                                                                    \
             PyErr_Format(ExcThreadingViolation, "You are trying to use the same object concurrently in two threads which is not allowed."); \
           return e;                                                                                                                 \
      }                                                                                                                              \
  }

/* used by Connection */
#define CHECK_CLOSED(connection,e) \
{ if(!connection->db) { PyErr_Format(ExcConnectionClosed, "The connection has been closed"); return e; } }


/* these two are used by Connection and Cursor */

#define APSW_BEGIN_ALLOW_THREADS \
  do { \
      assert(self->inuse==0); self->inuse=1; \
      Py_BEGIN_ALLOW_THREADS

#define APSW_END_ALLOW_THREADS \
     Py_END_ALLOW_THREADS; \
     assert(self->inuse==1); self->inuse=0; \
  } while(0)
