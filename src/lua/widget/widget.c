/*
   This file is part of darktable,
   copyright (c) 2015 Jeremy Rosen

   darktable is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   darktable is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "lua/widget/common.h"
#include "lua/widget/common.h"
#include "lua/types.h"
#include "lua/modules.h"
#include "lua/call.h"
#include "control/control.h"
#include "stdarg.h"
/**
  TODO
  generic property member registration
  use name to save/restore states as pref like other widgets
  have a way to save presets
  storage lib looses index for lua storages, 
  luastorage can't save presets

  */

dt_lua_widget_type_t widget_type = {
  .name = "widget",
  .gui_init = NULL,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent = NULL
};


static void init_widget_sub(lua_State *L,dt_lua_widget_type_t*widget_type);
static void init_widget_sub(lua_State *L,dt_lua_widget_type_t*widget_type) {
  if(widget_type->parent) 
    init_widget_sub(L,widget_type->parent);
  if(widget_type->gui_init) 
    widget_type->gui_init(L);
}
static int get_widget_params(lua_State *L)
{
  struct dt_lua_widget_type_t *widget_type = lua_touserdata(L, lua_upvalueindex(1));
  if(G_TYPE_IS_ABSTRACT(widget_type->gtk_type)){
    luaL_error(L,"Trying to create a widget of an abstract type : %s\n",widget_type->name);
  }
  lua_widget widget= malloc(widget_type->alloc_size);
  widget->widget = gtk_widget_new(widget_type->gtk_type,NULL);
  g_object_ref_sink(widget->widget);
  widget->type = widget_type;
  luaA_push_type(L,widget_type->associated_type,&widget);
  dt_lua_type_gpointer_alias_type(L,widget_type->associated_type,widget,widget->widget);
  init_widget_sub(L,widget_type);

  luaL_getmetafield(L,-1,"__gtk_signals");
  lua_pushnil(L); /* first key */
  while(lua_next(L, -2) != 0)
  {
    g_signal_connect(widget->widget, lua_tostring(L,-2), G_CALLBACK(lua_touserdata(L,-1)), widget);
    lua_pop(L,1);
  }
  lua_pop(L,1);
  return 1;
}

static int widget_gc(lua_State *L)
{
  lua_widget widget;
  luaA_to(L,lua_widget,&widget,1);
  if(widget->type->gui_cleanup) {
    widget->type->gui_cleanup(L,widget);
  }
  g_object_unref(widget->widget);
  free(widget);
  return 0;
}

luaA_Type dt_lua_init_widget_type_type(lua_State *L, dt_lua_widget_type_t* widget_type,const char* lua_type,GType gtk_type)
{
  luaA_Type type_id = dt_lua_init_gpointer_type_type(L,luaA_type_add(L,lua_type,sizeof(gpointer)));
  widget_type->associated_type = type_id;
  widget_type->gtk_type = gtk_type;
  dt_lua_type_register_parent_type(L, type_id, widget_type->parent->associated_type);

  lua_newtable(L);
  dt_lua_type_setmetafield_type(L,type_id,"__gtk_signals");
  // add to the table
  lua_pushlightuserdata(L, widget_type);
  lua_pushcclosure(L, get_widget_params, 1);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_module_entry_new(L, -1, "widget", widget_type->name);
  lua_pop(L, 1);
  return type_id;
};



static int new_widget(lua_State *L)
{
  const char *entry_name = luaL_checkstring(L, 1);
  dt_lua_module_entry_push(L, "widget", entry_name);
  lua_insert(L,2);
  lua_call(L, lua_gettop(L)-2, 1);
  return 1;
}

void dt_lua_widget_set_callback(lua_State *L,int index,const char* name)
{
  luaL_argcheck(L, dt_lua_isa(L, index, lua_widget), index, "lua_widget expected");
  luaL_checktype(L,-1,LUA_TFUNCTION);
  lua_getuservalue(L,index);
  lua_pushvalue(L,-2);
  lua_setfield(L,-2,name);
  lua_pop(L,2);
}

void dt_lua_widget_get_callback(lua_State *L,int index,const char* name)
{
  luaL_argcheck(L, dt_lua_isa(L, index, lua_widget), index, "lua_widget expected");
  lua_getuservalue(L,index);
  lua_getfield(L,-1,name);
  lua_remove(L,-2);
}


void dt_lua_widget_trigger_callback_glist(lua_State*L,lua_widget object,const char* name,GList*extra)
{
  luaA_push_type(L,object->type->associated_type,&object);
  lua_getuservalue(L,-1);
  lua_getfield(L,-1,name);
  if(! lua_isnil(L,-1)) {
    lua_pushvalue(L,-3);
    GList* cur_elt = extra;
    int nargs = 1;
    while(cur_elt) {
      const char* next_type = cur_elt->data;
      cur_elt = g_list_next(cur_elt);
      luaA_push_type(L,luaA_type_find(L,next_type),&cur_elt->data);
      nargs++;
      cur_elt = g_list_next(cur_elt);
    }
    dt_lua_do_chunk(L,nargs,0);
  }
  dt_lua_redraw_screen();
  g_list_free(extra);
  lua_pop(L,2);
}

void dt_lua_widget_trigger_callback(lua_State*L,lua_widget object,const char* name)
{
  dt_lua_widget_trigger_callback_glist(L,object,name,NULL);
}


typedef struct {
  lua_widget object;
  char * event_name;
  GList* extra;
}widget_callback_data;


static int32_t widget_callback_job(dt_job_t *job)
{
  dt_lua_lock();
  lua_State* L= darktable.lua_state.state;
  widget_callback_data* data = (widget_callback_data*)dt_control_job_get_params(job);
  dt_lua_widget_trigger_callback_glist(L,data->object,data->event_name,data->extra);
  free(data->event_name);
  free(data);
  dt_lua_unlock();
  return 0;

}

void dt_lua_widget_trigger_callback_async(lua_widget object,const char* name,const char* type_name,...)
{
  dt_job_t *job = dt_control_job_create(&widget_callback_job, "lua: widget event");
  if(job)
  {
    widget_callback_data*data = malloc(sizeof(widget_callback_data));
    data->object = object;
    data->event_name = strdup(name);
    data->extra=NULL;
    va_list ap;
    va_start(ap,type_name);
    const char *cur_type = type_name;
    while(cur_type ){
      data->extra=g_list_append(data->extra,GINT_TO_POINTER(cur_type));
      data->extra=g_list_append(data->extra,va_arg(ap,gpointer));
      cur_type = va_arg(ap,const char*);

    }
    va_end(ap);
    
    dt_control_job_set_params(job, data);
    dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
  }
}

static int reset_member(lua_State *L)
{
  if(lua_gettop(L) > 2) {
    dt_lua_widget_set_callback(L,1,"reset");
    return 0;
  }
  dt_lua_widget_get_callback(L,1,"reset");
  return 1;
}


static int tooltip_member(lua_State *L)
{
  lua_widget widget;
  luaA_to(L,lua_widget,&widget,1);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L,3)) {
      gtk_widget_set_tooltip_text(widget->widget,NULL);
    } else {
      const char * text = luaL_checkstring(L,3);
      gtk_widget_set_tooltip_text(widget->widget,text);
    }
    return 0;
  }
  char* result = gtk_widget_get_tooltip_text(widget->widget);
  lua_pushstring(L,result);
  free(result);
  return 1;
}

static int gtk_signal_member(lua_State *L)
{

  const char *signal = lua_tostring(L,lua_upvalueindex(1));
  if(lua_gettop(L) > 2) {
    dt_lua_widget_set_callback(L,1,signal);
    return 0;
  }
  dt_lua_widget_get_callback(L,1,signal);
  return 1;
}

void dt_lua_widget_register_gtk_callback_type(lua_State *L,luaA_Type type_id,const char* signal_name, const char* lua_name,GCallback callback) 
{
  lua_pushstring(L,signal_name);
  lua_pushcclosure(L,gtk_signal_member,1);
  dt_lua_type_register_type(L, type_id, lua_name);

  luaL_newmetatable(L, luaA_typename(L, type_id));
  lua_getfield(L,-1,"__gtk_signals");
  lua_pushlightuserdata(L,callback);
  lua_setfield(L,-2,signal_name);
  lua_pop(L,2);
  
}

int widget_call(lua_State *L)
{
  lua_pushnil(L); /* first key */
  while(lua_next(L, 2) != 0)
  {
    lua_pushvalue(L,-2);
    lua_pushvalue(L,-2);
    lua_settable(L,1);
    lua_pop(L,1);
  }
  lua_pop(L,1);
  return 1;
}

int dt_lua_init_widget(lua_State* L)
{
  dt_lua_module_new(L,"widget");

  widget_type.associated_type = dt_lua_init_gpointer_type(L,lua_widget);
  lua_pushcfunction(L,tooltip_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_widget, "tooltip");
  lua_pushcfunction(L,widget_gc);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_setmetafield(L,lua_widget,"__gc");
  lua_pushcfunction(L,reset_member);
  dt_lua_type_register(L, lua_widget, "reset_callback");
  lua_pushcfunction(L,widget_call);
  dt_lua_type_setmetafield(L,lua_widget,"__call");
  
  dt_lua_init_widget_container(L);
  dt_lua_init_widget_box(L);
  dt_lua_init_widget_button(L);
  dt_lua_init_widget_check_button(L);
  dt_lua_init_widget_combobox(L);
  dt_lua_init_widget_label(L);
  dt_lua_init_widget_entry(L);
  dt_lua_init_widget_file_chooser_button(L);
  dt_lua_init_widget_separator(L);

  luaA_enum(L,dt_lua_orientation_t);
  luaA_enum_value_name(L,dt_lua_orientation_t,GTK_ORIENTATION_HORIZONTAL,"horizontal");
  luaA_enum_value_name(L,dt_lua_orientation_t,GTK_ORIENTATION_VERTICAL,"vertical");



  dt_lua_push_darktable_lib(L);
  lua_pushstring(L, "new_widget");
  lua_pushcfunction(L, &new_widget);
  lua_settable(L, -3);
  lua_pop(L, 1);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;