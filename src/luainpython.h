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
#ifndef LUAINPYTHON_H
#define LUAINPYTHON_H

typedef struct {
	PyObject_HEAD
	PyObject *state;
	int ref;
	int refiter;
} LuaObject;

PyAPI_DATA(PyTypeObject) LuaObjectType;

#define LuaObject_Check(op) PyObject_TypeCheck(op, &LuaObjectType)

/* Type object to hold Lua state */
typedef struct {
	PyObject_HEAD
	lua_State *LuaState;
} LuaStateObject;

PyAPI_DATA(PyTypeObject) LuaStateObjectType;

PyObject *LuaConvert(LuaStateObject *state, int n);
LuaStateObject *GetGlobalLuaState(void);

DL_EXPORT(void) initlua(void);

#endif
