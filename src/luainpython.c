/*

 Lunatic Python
 --------------
 
 Copyright (c) 2002-2005  Gustavo Niemeyer <gustavo@niemeyer.net>

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <setjmp.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "pythoninlua.h"
#include "luainpython.h"

jmp_buf errjmp;

static int py_lua_panic(lua_State* LuaState)
{
	size_t len;
	const char *s = lua_tolstring(LuaState, -1, &len);
	PyObject *o = PyString_FromStringAndSize(s, len);
	PyErr_SetObject(PyExc_RuntimeError, o);
	longjmp(errjmp, -1);
	return (-1);
}

static void *py_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	(void)ud; (void)osize;
	if (nsize == 0) {
		PyMem_Free(ptr);
		return NULL;
	} else {
		return PyMem_Realloc(ptr, nsize);
	}
}

static int py_lua_module_panic(lua_State* LuaState)
{
	const char *s = lua_tostring(LuaState, -1);
	fprintf(stderr, "Error in lua module: %s\n", s);
	fprintf(stderr, "exiting (sorry)\n");
	fflush(stderr);
	return (-1);
}


#define TRY     do {							\
        lua_CFunction old_panic = lua_atpanic(LuaState, py_lua_panic);	\
	if (setjmp(errjmp) == 0) {
#define CATCH    lua_atpanic(LuaState, old_panic);	\
        } else {					\
		  lua_atpanic(LuaState, old_panic);
#define ENDTRY   }} while (0)


lua_State *LuaState = NULL;

/*********************************************************************************
 * Lua object
 ********************************************************************************/

static PyObject *LuaObject_New(int n);

PyObject *LuaConvert(lua_State *LuaState, int n)
{
	PyObject *ret = NULL;

	switch (lua_type(LuaState, n)) {

		case LUA_TNIL:
			Py_INCREF(Py_None);
			ret = Py_None;
			break;

		case LUA_TSTRING: {
			size_t len;
			const char *s = lua_tolstring(LuaState, n, &len);
			ret = PyString_FromStringAndSize(s, len);
			break;
		}

		case LUA_TNUMBER: {
			lua_Number num = lua_tonumber(LuaState, n);
#ifdef LUA_NUMBER_DOUBLE
			if (num != (long)num) {
				ret = PyFloat_FromDouble(n);
			} else
#endif
			{
				ret = PyInt_FromLong((long)num);
			}
			break;
		}

		case LUA_TBOOLEAN:
			if (lua_toboolean(LuaState, n)) {
				Py_INCREF(Py_True);
				ret = Py_True;
			} else {
				Py_INCREF(Py_False);
				ret = Py_False;
			}
			break;

		case LUA_TUSERDATA: {
			py_object *obj = check_py_object(LuaState, n);

			if (obj) {
				Py_INCREF(obj->o);
				ret = obj->o;
				break;
			}

			/* Otherwise go on and handle as custom. */
		}

		default:
			ret = LuaObject_New(n);
			break;
	}

	return ret;
}

static int e_py_convert(lua_State *LuaState, PyObject *o, int withnone)
{
	int r = 0;
	TRY {
		r = py_convert(LuaState, o, withnone);
	} CATCH {
		r = 0;
	} ENDTRY;
	if (!r) {
		PyErr_SetString(PyExc_RuntimeError, "can't convert");
	}
	return r;
}

static PyObject *LuaCall(lua_State *LuaState, PyObject *args)
{
	PyObject *ret = NULL;
	PyObject *arg;
	int nargs, rc, i;

	assert(PyTuple_Check(args));

	nargs = PyTuple_Size(args);
	for (i = 0; i != nargs; i++) {
		arg = PyTuple_GetItem(args, i);
		if (arg == NULL) {
			PyErr_Format(PyExc_TypeError,
				     "failed to get tuple item #%d", i);
			lua_settop(LuaState, 0);
			return NULL;
		}
		rc = e_py_convert(LuaState, arg, 0);
		if (!rc) {
			PyErr_Format(PyExc_TypeError,
				     "failed to convert argument #%d", i);
			lua_settop(LuaState, 0);
			return NULL;
		}
	}

	if (lua_pcall(LuaState, nargs, LUA_MULTRET, 0) != 0) {
		PyErr_Format(PyExc_Exception,
			     "error: %s", lua_tostring(LuaState, -1));
		return NULL;
	}

	nargs = lua_gettop(LuaState);
	if (nargs == 1) {
		ret = LuaConvert(LuaState, 1);
		if (!ret) {
			PyErr_SetString(PyExc_TypeError,
				        "failed to convert return");
			lua_settop(LuaState, 0);
			Py_DECREF(ret);
			return NULL;
		}
	} else if (nargs > 1) {
		ret = PyTuple_New(nargs);
		if (!ret) {
			PyErr_SetString(PyExc_RuntimeError,
					"failed to create return tuple");
			lua_settop(LuaState, 0);
			return NULL;
		}
		for (i = 0; i != nargs; i++) {
			arg = LuaConvert(LuaState, i+1);
			if (!arg) {
				PyErr_Format(PyExc_TypeError,
					     "failed to convert return #%d", i);
				lua_settop(LuaState, 0);
				Py_DECREF(ret);
				return NULL;
			}
			PyTuple_SetItem(ret, i, arg);
		}
	} else {
		Py_INCREF(Py_None);
		ret = Py_None;
	}
	
	lua_settop(LuaState, 0);

	return ret;
}

static PyObject *LuaObject_New(int n)
{
	LuaObject *obj = PyObject_New(LuaObject, &LuaObject_Type);
	if (obj) {
		lua_pushvalue(LuaState, n);
		obj->ref = luaL_ref(LuaState, LUA_REGISTRYINDEX);
		obj->refiter = LUA_NOREF;
	}
	return (PyObject*) obj;
}

static void LuaObject_dealloc(LuaObject *self)
{
	luaL_unref(LuaState, LUA_REGISTRYINDEX, self->ref);
	if (self->refiter != LUA_NOREF)
		luaL_unref(LuaState, LUA_REGISTRYINDEX, self->refiter);
	self->ob_type->tp_free((PyObject *)self);
}

static PyObject *LuaObject_getattr(PyObject *obj, PyObject *attr)
{
	PyObject *ret = NULL;
	int rc;
	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, ((LuaObject*)obj)->ref);
	if (lua_isnil(LuaState, -1)) {
		PyErr_SetString(PyExc_RuntimeError, "lost reference");
		goto error;
	}
	rc = e_py_convert(LuaState, attr, 0);
	if (rc) {
		TRY {
			lua_gettable(LuaState, -2);
		} CATCH {
			goto error;
		} ENDTRY;

		ret = LuaConvert(LuaState, -1);
	} else {
		PyErr_SetString(PyExc_ValueError, "can't convert attr/key");
	}
  error:
	lua_settop(LuaState, 0);
	return ret;
}

static int LuaObject_setattr(PyObject *obj, PyObject *attr, PyObject *value)
{
	int ret = -1;
	int rc;
	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, ((LuaObject*)obj)->ref);
	if (lua_isnil(LuaState, -1)) {
		PyErr_SetString(PyExc_RuntimeError, "lost reference");
		goto error;
	}
	if (!lua_istable(LuaState, -1)) {
		PyErr_SetString(PyExc_TypeError, "Lua object is not a table");
		goto error;
	}
	rc = e_py_convert(LuaState, attr, 0);
	if (rc) {
		rc = e_py_convert(LuaState, value, 0);
		if (rc) {
			TRY {
				lua_settable(LuaState, -3);
			} CATCH {
				ret = -1;
				goto error;
			} ENDTRY;
			ret = 0;
		} else {
			PyErr_SetString(PyExc_ValueError,
					"can't convert value");
		}
	} else {
		PyErr_SetString(PyExc_ValueError, "can't convert key/attr");
	}
  error:
	lua_settop(LuaState, 0);
	return ret;
}

static PyObject *LuaObject_str(PyObject *obj)
{
	PyObject *ret = NULL;
	const char *s;
	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, ((LuaObject*)obj)->ref);
	int r = 0;
	TRY {
		r = luaL_callmeta(LuaState, -1, "__tostring");
	} CATCH { 
	} ENDTRY;
	if (r) {
		s = lua_tostring(LuaState, -1);
		lua_pop(LuaState, 1);
		if (s) ret = PyString_FromString(s);
	}
	if (!ret) {
		int type = lua_type(LuaState, -1);
		switch (type) {
			case LUA_TTABLE:
			case LUA_TFUNCTION:
				ret = PyString_FromFormat("<Lua %s at %p/r=%d>",
							  lua_typename(LuaState, type),
							  lua_topointer(LuaState, -1),
							  ((LuaObject*)obj)->ref);
				break;
			
			case LUA_TUSERDATA:
			case LUA_TLIGHTUSERDATA:
				ret = PyString_FromFormat("<Lua %s at %p>",
					lua_typename(LuaState, type),
					lua_touserdata(LuaState, -1));
				break;

			case LUA_TTHREAD:
				ret = PyString_FromFormat("<Lua %s at %p>",
					lua_typename(LuaState, type),
					(void*)lua_tothread(LuaState, -1));
				break;

			default:
				ret = PyString_FromFormat("<Lua %s>",
					lua_typename(LuaState, type));
				break;

		}
	}
	lua_pop(LuaState, 1);
	return ret;
}

static PyObject *LuaObject_call(PyObject *obj, PyObject *args)
{
	lua_settop(LuaState, 0);
	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, ((LuaObject*)obj)->ref);
	return LuaCall(LuaState, args);
}

static PyObject *LuaObject_iternext(LuaObject *obj)
{
	PyObject *ret = NULL;

	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, ((LuaObject*)obj)->ref);

	if (obj->refiter == LUA_NOREF)
		lua_pushnil(LuaState);
	else
		lua_rawgeti(LuaState, LUA_REGISTRYINDEX, obj->refiter);

	int r = 0;
	TRY {
		r = lua_next(LuaState, -2);
	} CATCH {
	} ENDTRY;
	if (r != 0) {
		/* Remove value. */
		lua_pop(LuaState, 1);
		ret = LuaConvert(LuaState, -1);
		/* Save key for next iteration. */
		if (obj->refiter == LUA_NOREF)
			obj->refiter = luaL_ref(LuaState, LUA_REGISTRYINDEX);
		else
			lua_rawseti(LuaState, LUA_REGISTRYINDEX, obj->refiter);
	} else if (obj->refiter != LUA_NOREF) {
		luaL_unref(LuaState, LUA_REGISTRYINDEX, obj->refiter);
		obj->refiter = LUA_NOREF;
	}

	return ret;
}

static Py_ssize_t LuaObject_length(LuaObject *obj)
{
	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, ((LuaObject*)obj)->ref);
	size_t len = lua_objlen(LuaState, -1);
	lua_settop(LuaState, 0);
	return len;
}

static PyObject *LuaObject_subscript(PyObject *obj, PyObject *key)
{
	return LuaObject_getattr(obj, key);
}

static int LuaObject_ass_subscript(PyObject *obj,
				   PyObject *key, PyObject *value)
{
	return LuaObject_setattr(obj, key, value);
}

static PyMappingMethods LuaObject_as_mapping = {
	(lenfunc)LuaObject_length,	/*mp_length*/
	(binaryfunc)LuaObject_subscript,/*mp_subscript*/
	(objobjargproc)LuaObject_ass_subscript,/*mp_ass_subscript*/
};

PyTypeObject LuaObject_Type = {
	PyObject_HEAD_INIT(NULL)
	0,			/*ob_size*/
	"lua.custom",		/*tp_name*/
	sizeof(LuaObject),	/*tp_basicsize*/
	0,			/*tp_itemsize*/
	(destructor)LuaObject_dealloc, /*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	LuaObject_str,		/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	&LuaObject_as_mapping,	/*tp_as_mapping*/
	0,			/*tp_hash*/
	(ternaryfunc)LuaObject_call,	      /*tp_call*/
	LuaObject_str,		 /*tp_str*/
	LuaObject_getattr,	/*tp_getattro*/
	LuaObject_setattr,	/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
	"Lua bridge object",	/*tp_doc*/
	0,			/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	PyObject_SelfIter,	/*tp_iter*/
	(iternextfunc)LuaObject_iternext, /*tp_iternext*/
	0,       		/*tp_methods*/
	0,       		/*tp_members*/
	0,                      /*tp_getset*/
	0,                      /*tp_base*/
	0,                      /*tp_dict*/
	0,                      /*tp_descr_get*/
	0,                      /*tp_descr_set*/
	0,                      /*tp_dictoffset*/
	0,			/*tp_init*/
	0,			/*tp_alloc*/
	0,			/*tp_new*/
	0,			/*tp_free*/
	0,                      /*tp_is_gc*/
};

/*********************************************************************************
 * State object
 ********************************************************************************/

static void LuaStateObject_init(LuaStateObject *self)
{
	lua_State *NewLuaState = NULL;
	
	/* Create new Lua state */
	NewLuaState = lua_newstate(py_lua_alloc, py_lua_module_panic);
	
	/* Open libraries for the state */
	luaL_openlibs(NewLuaState);
	
	/* Reset state stack */
	lua_settop(NewLuaState, 0);
	
	/* Store Lua state in wrapper */
	self->LuaState = NewLuaState;
	
}

static void LuaStateObject_dealloc(LuaStateObject *self)
{
	if (self->LuaState) {
		lua_close(self->LuaState);
		self->LuaState = NULL;
	}
	self->ob_type->tp_free((PyObject *)self);
}

static PyObject *LuaStateObject_getattr(PyObject *obj, PyObject *attr)
{
	PyObject *ret = NULL;
	return ret;
}

static int LuaStateObject_setattr(PyObject *obj, PyObject *attr, PyObject *value)
{
	PyErr_SetString(PyExc_RuntimeError, "can't set attributes of Lua state");
	return -1;
}

static PyObject *LuaStateObject_str(PyObject *obj)
{
	return PyString_FromFormat("<LuaState %p at %p>", ((LuaStateObject *)obj)->LuaState, obj);
}

static PyObject *LuaStateObject_call(PyObject *obj, PyObject *args)
{
	//lua_settop(LuaState, 0);
	//lua_rawgeti(LuaState, LUA_REGISTRYINDEX, ((LuaObject*)obj)->ref);
	//return LuaCall(LuaState, args);
	return NULL;
}

PyObject *LuaState_run(LuaStateObject *self, PyObject *args, int eval)
{
	PyObject *ret = NULL;
	char *buf = NULL;
	char *s;
	Py_ssize_t len;

	if (!PyArg_ParseTuple(args, "s#", &s, &len))
		goto error;

	if (eval) {
		size_t retlen = sizeof("return ")-1;
		size_t lenbuf = retlen + len;
		buf = PyMem_New(char, lenbuf+1);
		strcpy(buf, "return ");
		memcpy(buf+retlen, s, len);
		buf[lenbuf] = '\0';
		s = buf;
		len = lenbuf;
	}

	if (luaL_loadbuffer(self->LuaState, s, len, "<python>") != 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "error loading code: %s",
			     lua_tostring(self->LuaState, -1));
		goto error;
	}

	PyMem_Free(buf);
	
	if (lua_pcall(self->LuaState, 0, 1, 0) != 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "error executing code: %s",
			     lua_tostring(self->LuaState, -1));
		goto error;
	}

	ret = LuaConvert(self->LuaState, -1);
  error:
	lua_settop(self->LuaState, 0);
	return ret;
}

PyObject *LuaState_execute(PyObject *pself, PyObject *args)
{
	LuaStateObject *self = (LuaStateObject *)pself;
	return LuaState_run(self, args, 0);
}

PyObject *LuaState_eval(PyObject *pself, PyObject *args)
{
	LuaStateObject *self = (LuaStateObject *)pself;
	return LuaState_run(self, args, 1);
}

PyObject *LuaState_globals(PyObject *pself, PyObject *args)
{
	LuaStateObject *self = (LuaStateObject *)pself;
	PyObject *ret = NULL;
	lua_pushvalue(self->LuaState, LUA_GLOBALSINDEX);
	if (lua_isnil(self->LuaState, -1)) {
		PyErr_SetString(PyExc_RuntimeError,
				"lost globals reference");
		lua_pop(self->LuaState, 1);
		return NULL;
	}
	ret = LuaConvert(self->LuaState, -1);
	if (!ret)
		PyErr_Format(PyExc_TypeError,
			     "failed to convert globals table");
	lua_settop(self->LuaState, 0);
	return ret;
}

static PyObject *LuaState_require(PyObject *pself, PyObject *args)
{
	LuaStateObject *self = (LuaStateObject *)pself;
	lua_pushliteral(self->LuaState, "require");
	lua_rawget(self->LuaState, LUA_GLOBALSINDEX);
	if (lua_isnil(self->LuaState, -1)) {
		lua_pop(self->LuaState, 1);
		PyErr_SetString(PyExc_RuntimeError, "require is not defined");
		return NULL;
	}
	return LuaCall(self->LuaState, args);
}
static PyMethodDef luastate_methods[] = {
	{"execute",	LuaState_execute,	METH_VARARGS,		NULL},
	{"eval",	LuaState_eval,		METH_VARARGS,		NULL},
	{"globals",	LuaState_globals,	METH_NOARGS,		NULL},
	{"require", 	LuaState_require,	METH_VARARGS,		NULL},
	{NULL,		NULL}
};

/* Python type object to hold LuaState */
PyTypeObject LuaStateObjectType = {
	PyObject_HEAD_INIT(NULL)
	0,			/*ob_size*/
	"lua.LuaState",		/*tp_name*/
	sizeof(LuaStateObject),	/*tp_basicsize*/
	0,			/*tp_itemsize*/
	(destructor)LuaStateObject_dealloc, /*tp_dealloc*/
	0,			/*tp_print*/
	0,			/*tp_getattr*/
	0,			/*tp_setattr*/
	0,			/*tp_compare*/
	LuaStateObject_str,		/*tp_repr*/
	0,			/*tp_as_number*/
	0,			/*tp_as_sequence*/
	0,	/*tp_as_mapping*/
	0,			/*tp_hash*/
	(ternaryfunc)LuaStateObject_call,	      /*tp_call*/
	LuaStateObject_str,		 /*tp_str*/
	0,//LuaStateObject_getattr,	/*tp_getattro*/
	0,//LuaStateObject_setattr,	/*tp_setattro*/
	0,			/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
	"Lua state object",	/*tp_doc*/
	0,			/*tp_traverse*/
	0,			/*tp_clear*/
	0,			/*tp_richcompare*/
	0,			/*tp_weaklistoffset*/
	0,	/*tp_iter*/
	0, /*tp_iternext*/
	0,//luastate_methods,       		/*tp_methods*/
	0,       		/*tp_members*/
	0,                      /*tp_getset*/
	0,                      /*tp_base*/
	0,                      /*tp_dict*/
	0,                      /*tp_descr_get*/
	0,                      /*tp_descr_set*/
	0,                      /*tp_dictoffset*/
	(initproc)LuaStateObject_init,			/*tp_init*/
	0,			/*tp_alloc*/
	0,			/*tp_new*/
	0,			/*tp_free*/
	0,                      /*tp_is_gc*/
};

/*********************************************************************************
 * Module
 ********************************************************************************/


PyObject *Lua_run(PyObject *args, int eval)
{
	PyObject *ret = NULL;
	char *buf = NULL;
	char *s;
	Py_ssize_t len;

	if (!PyArg_ParseTuple(args, "s#", &s, &len))
		goto error;

	if (eval) {
		size_t retlen = sizeof("return ")-1;
		size_t lenbuf = retlen + len;
		buf = PyMem_New(char, lenbuf+1);
		strcpy(buf, "return ");
		memcpy(buf+retlen, s, len);
		buf[lenbuf] = '\0';
		s = buf;
		len = lenbuf;
	}

	if (luaL_loadbuffer(LuaState, s, len, "<python>") != 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "error loading code: %s",
			     lua_tostring(LuaState, -1));
		goto error;
	}

	PyMem_Free(buf);
	
	if (lua_pcall(LuaState, 0, 1, 0) != 0) {
		PyErr_Format(PyExc_RuntimeError,
			     "error executing code: %s",
			     lua_tostring(LuaState, -1));
		goto error;
	}

	ret = LuaConvert(LuaState, -1);
  error:
	lua_settop(LuaState, 0);
	return ret;
}

PyObject *Lua_execute(PyObject *self, PyObject *args)
{
	return Lua_run(args, 0);
}

PyObject *Lua_eval(PyObject *self, PyObject *args)
{
	return Lua_run(args, 1);
}

PyObject *Lua_globals(PyObject *self, PyObject *args)
{
	PyObject *ret = NULL;
	lua_pushvalue(LuaState, LUA_GLOBALSINDEX);
	if (lua_isnil(LuaState, -1)) {
		PyErr_SetString(PyExc_RuntimeError,
				"lost globals reference");
		lua_pop(LuaState, 1);
		return NULL;
	}
	ret = LuaConvert(LuaState, -1);
	if (!ret)
		PyErr_Format(PyExc_TypeError,
			     "failed to convert globals table");
	lua_settop(LuaState, 0);
	return ret;
}

static PyObject *Lua_require(PyObject *self, PyObject *args)
{
	lua_pushliteral(LuaState, "require");
	lua_rawget(LuaState, LUA_GLOBALSINDEX);
	if (lua_isnil(LuaState, -1)) {
		lua_pop(LuaState, 1);
		PyErr_SetString(PyExc_RuntimeError, "require is not defined");
		return NULL;
	}
	return LuaCall(LuaState, args);
}

/**
 * Create a new LuaState which can have its own global variables
 * independently of the module-wide state.
 */
static PyObject *Lua_new_state(PyObject *self, PyObject *args)
{
	return PyObject_CallObject((PyObject *)&LuaStateObjectType, NULL);
}

static PyMethodDef lua_methods[] = {
	{"execute",	Lua_execute,	METH_VARARGS,		NULL},
	{"eval",	Lua_eval,	METH_VARARGS,		NULL},
	{"globals",	Lua_globals,	METH_NOARGS,		NULL},
	{"require", 	Lua_require,	METH_VARARGS,		NULL},
	{"new_state",	Lua_new_state,	METH_NOARGS,		NULL},
	{NULL,		NULL}
};

DL_EXPORT(void)
initlua(void)
{
	PyObject *m;

	LuaObject_Type.tp_new = PyType_GenericNew;
	if (PyType_Ready(&LuaObject_Type) < 0)
		return;
	
	LuaStateObjectType.tp_new = PyType_GenericNew;
	if (PyType_Ready(&LuaStateObjectType) < 0)
		return;

	m = Py_InitModule3("lua", lua_methods,
			   "Lua as a Python module (with state support).");
	if (!m)
		return;
	
	Py_INCREF(&LuaStateObjectType);
	PyModule_AddObject(m, "LuaState", (PyObject *)&LuaStateObjectType);
	
	if (!LuaState) {
		LuaState = lua_newstate(py_lua_alloc, py_lua_module_panic);
		luaL_openlibs(LuaState);
		if (lua_cpcall(LuaState, luaopen_python, NULL) != 0) {
			PyErr_SetString(PyExc_RuntimeError,
					"can't open python lib in lua");
		}
		lua_settop(LuaState, 0);
	}
}
