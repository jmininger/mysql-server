/*
  Copyright (c) 2005, 2016, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "events.h"

#include "m_ctype.h"               // CHARSET_INFO
#include "auth_common.h"           // EVENT_ACL
#include "event_data_objects.h"    // Event_queue_element
#include "event_db_repository.h"   // Event_db_repository
#include "event_parse_data.h"      // Event_parse_data
#include "event_queue.h"           // Event_queue
#include "event_scheduler.h"       // Event_scheduler
#include "lock.h"                  // lock_object_name
#include "log.h"                   // sql_print_error
#include "mysqld.h"                // LOCK_global_system_variables
#include "mysqld_error.h"          // ER_*
#include "records.h"               // READ_RECORD
#include "sp_head.h"               // Stored_program_creation_ctx
#include "sql_base.h"              // close_mysql_tables
#include "sql_class.h"             // THD
#include "sql_show.h"              // append_definer
#include "sql_string.h"            // String
#include "sql_table.h"             // write_bin_log
#include "tztime.h"                // Time_zone
#include "transaction.h"

#include "dd/dd_schema.h"               // dd::Schema_MDL_locker
#include "dd/dd_event.h"
#include "dd/iterator.h"                // dd::Iterator
#include "dd/cache/dictionary_client.h"

#include "mysql/psi/mysql_sp.h"
#include "mysql/psi/mysql_memory.h"


/**
  @addtogroup Event_Scheduler
  @{
*/

/*
 TODO list :
 - CREATE EVENT should not go into binary log! Does it now? The SQL statements
   issued by the EVENT are replicated.
   I have an idea how to solve the problem at failover. So the status field
   will be ENUM('DISABLED', 'ENABLED', 'SLAVESIDE_DISABLED').
   In this case when CREATE EVENT is replicated it should go into the binary
   as SLAVESIDE_DISABLED if it is ENABLED, when it's created as DISABLEd it
   should be replicated as disabled. If an event is ALTERed as DISABLED the
   query should go untouched into the binary log, when ALTERed as enable then
   it should go as SLAVESIDE_DISABLED. This is regarding the SQL interface.
   TT routines however modify mysql.event internally and this does not go the
   log so in this case queries has to be injected into the log...somehow... or
   maybe a solution is RBR for this case, because the event may go only from
   ENABLED to DISABLED status change and this is safe for replicating. As well
   an event may be deleted which is also safe for RBR.

 - Add logging to file

*/


/*
  If the user (un)intentionally removes an event directly from mysql.event
  the following sequence has to be used to be able to remove the in-memory
  counterpart.
  1. CREATE EVENT the_name ON SCHEDULE EVERY 1 SECOND DISABLE DO SELECT 1;
  2. DROP EVENT the_name

  In other words, the first one will create a row in mysql.event . In the
  second step because there will be a line, disk based drop will pass and
  the scheduler will remove the memory counterpart. The reason is that
  in-memory queue does not check whether the event we try to drop from memory
  is disabled. Disabled events are not kept in-memory because they are not
  eligible for execution.
*/

Event_queue *Events::event_queue;
Event_scheduler *Events::scheduler;
Event_db_repository *Events::db_repository;
ulong Events::opt_event_scheduler= Events::EVENTS_OFF;

static bool load_events_from_db(THD *thd, Event_queue *event_queue);

/*
  Compares 2 LEX strings regarding case.

  SYNOPSIS
    sortcmp_lex_string()
      s   First LEX_STRING
      t   Second LEX_STRING
      cs  Charset

  RETURN VALUE
   -1   s < t
    0   s == t
    1   s > t
*/

int sortcmp_lex_string(LEX_STRING s, LEX_STRING t, CHARSET_INFO *cs)
{
 return cs->coll->strnncollsp(cs, (uchar *) s.str,s.length,
                                  (uchar *) t.str,t.length);
}


void convert_name_lowercase(const char *from, char *to, size_t len)
{
  my_stpncpy(to, from, len - 1);
  my_casedn_str(&my_charset_utf8_tolower_ci, to);
  to[len - 1]= '\0';
}


/*
  Reconstructs interval expression from interval type and expression
  value that is in form of a value of the smalles entity:
  For
    YEAR_MONTH - expression is in months
    DAY_MINUTE - expression is in minutes

  SYNOPSIS
    Events::reconstruct_interval_expression()
      buf         Preallocated String buffer to add the value to
      interval    The interval type (for instance YEAR_MONTH)
      expression  The value in the lowest entity

  RETURN VALUE
    0  OK
    1  Error
*/

int
Events::reconstruct_interval_expression(String *buf, interval_type interval,
                                        longlong expression)
{
  ulonglong expr= expression;
  char tmp_buff[128], *end;
  bool close_quote= true;
  int multipl= 0;
  char separator=':';

  switch (interval) {
  case INTERVAL_YEAR_MONTH:
    multipl= 12;
    separator= '-';
    goto common_1_lev_code;
  case INTERVAL_DAY_HOUR:
    multipl= 24;
    separator= ' ';
    goto common_1_lev_code;
  case INTERVAL_HOUR_MINUTE:
  case INTERVAL_MINUTE_SECOND:
    multipl= 60;
common_1_lev_code:
    buf->append('\'');
    end= longlong10_to_str(expression/multipl, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));
    expr= expr - (expr/multipl)*multipl;
    break;
  case INTERVAL_DAY_MINUTE:
  {
    ulonglong tmp_expr= expr;

    tmp_expr/=(24*60);
    buf->append('\'');
    end= longlong10_to_str(tmp_expr, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));// days
    buf->append(' ');

    tmp_expr= expr - tmp_expr*(24*60);//minutes left
    end= longlong10_to_str(tmp_expr/60, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));// hours

    expr= tmp_expr - (tmp_expr/60)*60;
    /* the code after the switch will finish */
  }
    break;
  case INTERVAL_HOUR_SECOND:
  {
    ulonglong tmp_expr= expr;

    buf->append('\'');
    end= longlong10_to_str(tmp_expr/3600, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));// hours
    buf->append(':');

    tmp_expr= tmp_expr - (tmp_expr/3600)*3600;
    end= longlong10_to_str(tmp_expr/60, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));// minutes

    expr= tmp_expr - (tmp_expr/60)*60;
    /* the code after the switch will finish */
  }
    break;
  case INTERVAL_DAY_SECOND:
  {
    ulonglong tmp_expr= expr;

    tmp_expr/=(24*3600);
    buf->append('\'');
    end= longlong10_to_str(tmp_expr, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));// days
    buf->append(' ');

    tmp_expr= expr - tmp_expr*(24*3600);//seconds left
    end= longlong10_to_str(tmp_expr/3600, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));// hours
    buf->append(':');

    tmp_expr= tmp_expr - (tmp_expr/3600)*3600;
    end= longlong10_to_str(tmp_expr/60, tmp_buff, 10);
    buf->append(tmp_buff, (uint) (end- tmp_buff));// minutes

    expr= tmp_expr - (tmp_expr/60)*60;
    /* the code after the switch will finish */
  }
    break;
  case INTERVAL_DAY_MICROSECOND:
  case INTERVAL_HOUR_MICROSECOND:
  case INTERVAL_MINUTE_MICROSECOND:
  case INTERVAL_SECOND_MICROSECOND:
  case INTERVAL_MICROSECOND:
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "MICROSECOND");
    return 1;
    break;
  case INTERVAL_QUARTER:
    expr/= 3;
    close_quote= false;
    break;
  case INTERVAL_WEEK:
    expr/= 7;
  default:
    close_quote= false;
    break;
  }
  if (close_quote)
    buf->append(separator);
  end= longlong10_to_str(expr, tmp_buff, 10);
  buf->append(tmp_buff, (uint) (end- tmp_buff));
  if (close_quote)
    buf->append('\'');

  return 0;
}


/**
  Create a new query string for removing executable comments 
  for avoiding leak and keeping consistency of the execution 
  on master and slave.
  
  @param[in] thd                 Thread handler
  @param[in] buf                 Query string

  @return
             0           ok
             1           error
*/
static int
create_query_string(THD *thd, String *buf)
{
  /* Append the "CREATE" part of the query */
  if (buf->append(STRING_WITH_LEN("CREATE ")))
    return 1;
  /* Append definer */
  append_definer(thd, buf, thd->lex->definer->user, thd->lex->definer->host);
  /* Append the left part of thd->query after "DEFINER" part */
  if (buf->append(thd->lex->stmt_definition_begin,
                  thd->lex->stmt_definition_end -
                  thd->lex->stmt_definition_begin))
    return 1;
 
  return 0;
}


/**
  Create a new event.

  @param[in,out]  thd            THD
  @param[in]      parse_data     Event's data from parsing stage
  @param[in]      if_not_exists  Whether IF NOT EXISTS was
                                 specified
  In case there is an event with the same name (db) and
  IF NOT EXISTS is specified, an warning is put into the stack.
  @sa Events::drop_event for the notes about locking, pre-locking
  and Events DDL.

  @retval  false  OK
  @retval  true   Error (reported)
*/

bool
Events::create_event(THD *thd, Event_parse_data *parse_data,
                     bool if_not_exists)
{
  bool ret;
  bool event_already_exists;
  DBUG_ENTER("Events::create_event");

  DBUG_EXECUTE_IF("thd_killed_injection",
                   thd->killed= THD::KILL_QUERY;
                   DBUG_RETURN(false););

  /*
    Perform semantic checks outside of Event_db_repository:
    once CREATE EVENT is supported in prepared statements, the
    checks will be moved to PREPARE phase.
  */
  if (parse_data->check_parse_data(thd))
    DBUG_RETURN(true);

  /* At create, one of them must be set */
  DBUG_ASSERT(parse_data->expression || parse_data->execute_at);

  if (check_access(thd, EVENT_ACL, parse_data->dbname.str, NULL, NULL, 0, 0))
    DBUG_RETURN(true);

  // Acquire exclusive MDL lock.
  if (lock_object_name(thd, MDL_key::EVENT, parse_data->dbname.str,
                       parse_data->name.str))
    DBUG_RETURN(true);

  if (parse_data->do_not_create)
    DBUG_RETURN(false);
  /*
    Turn off row binlogging of this statement and use statement-based
    so that all supporting tables are updated for CREATE EVENT command.
    When we are going out of the function scope, the original binary
    format state will be restored.
  */
  Save_and_Restore_binlog_format_state binlog_format_state(thd);

  /* On error conditions my_error() is called so no need to handle here */
  if (!(ret= db_repository->create_event(thd, parse_data, if_not_exists,
                                         &event_already_exists)))
  {
    Event_queue_element *new_element;
    bool dropped= 0;

    if (opt_event_scheduler != Events::EVENTS_DISABLED && !event_already_exists)
    {
      if (!(new_element= new Event_queue_element()))
        ret= true;                                // OOM
      else if ((ret= db_repository->load_named_event(thd, parse_data->dbname,
                                                     parse_data->name,
                                                     new_element)))
      {
        if (!db_repository->drop_event(thd, parse_data->dbname, parse_data->name,
                                       true))
          dropped= 1;
        delete new_element;
      }
      else
      {
        /* TODO: do not ignore the out parameter and a possible OOM error! */
        bool created;
        if (event_queue)
          event_queue->create_event(thd, new_element, &created);
      }
    }
    /*
      binlog the create event unless it's been successfully dropped
    */
    if (!dropped)
    {
      /* Binlog the create event. */
      DBUG_ASSERT(thd->query().str && thd->query().length);
      String log_query;
      if (create_query_string(thd, &log_query))
      {
        sql_print_error("Event Error: An error occurred while creating query string, "
                        "before writing it into binary log.");
        ret= true;
      }
      else
      {
        thd->add_to_binlog_accessed_dbs(parse_data->dbname.str);
        /*
          If the definer is not set or set to CURRENT_USER, the value of CURRENT_USER
          will be written into the binary log as the definer for the SQL thread.
        */
        ret= write_bin_log(thd, true, log_query.c_ptr(), log_query.length());
      }
    }
  }
  DBUG_RETURN(ret);
}


/**
  Alter an event.

  @param[in,out] thd         THD
  @param[in]     parse_data  Event's data from parsing stage
  @param[in]     new_dbname  A new schema name for the event. Set in the case of
                             ALTER EVENT RENAME, otherwise is NULL.
  @param[in]     new_name    A new name for the event. Set in the case of
                             ALTER EVENT RENAME

  Parameter 'et' contains data about dbname and event name.
  Parameter 'new_name' is the new name of the event, if not null
  this means that RENAME TO was specified in the query
  @sa Events::drop_event for the locking notes.

  @retval  false  OK
  @retval  true   error (reported)
*/

bool
Events::update_event(THD *thd, Event_parse_data *parse_data,
                     LEX_STRING *new_dbname, LEX_STRING *new_name)
{
  int ret;
  Event_queue_element *new_element;

  DBUG_ENTER("Events::update_event");

  if (parse_data->check_parse_data(thd) || parse_data->do_not_create)
    DBUG_RETURN(true);

  if (check_access(thd, EVENT_ACL, parse_data->dbname.str, NULL, NULL, 0, 0))
    DBUG_RETURN(true);

  if (lock_object_name(thd, MDL_key::EVENT, parse_data->dbname.str,
                       parse_data->name.str))
    DBUG_RETURN(true);

  if (new_dbname != NULL)                               /* It's a rename */
  {
    /* Check that the new and the old names differ. */
    if ( !sortcmp_lex_string(parse_data->dbname, *new_dbname,
                             system_charset_info) &&
         !sortcmp_lex_string(parse_data->name, *new_name,
                             system_charset_info))
    {
      my_error(ER_EVENT_SAME_NAME, MYF(0));
      DBUG_RETURN(true);
    }

    /*
      And the user has sufficient privileges to use the target database.
      Do it before checking whether the database exists: we don't want
      to tell the user that a database doesn't exist if they can not
      access it.
    */
    if (check_access(thd, EVENT_ACL, new_dbname->str, NULL, NULL, 0, 0))
      DBUG_RETURN(true);

    //  Acquire mdl exclusive lock on target database name.
    if (lock_object_name(thd, MDL_key::EVENT, new_dbname->str, new_name->str))
      DBUG_RETURN(true);
  }

  /*
    Turn off row binlogging of this statement and use statement-based
    so that all supporting tables are updated for CREATE EVENT command.
    When we are going out of the function scope, the original binary
    format state will be restored.
  */
  Save_and_Restore_binlog_format_state binlog_format_state(thd);

  /* On error conditions my_error() is called so no need to handle here */
  if (!(ret= db_repository->update_event(thd, parse_data,
                                         new_dbname, new_name)))
  {
    LEX_STRING dbname= new_dbname ? *new_dbname : parse_data->dbname;
    LEX_STRING name= new_name ? *new_name : parse_data->name;

    if (opt_event_scheduler != Events::EVENTS_DISABLED)
    {
      if (!(new_element= new Event_queue_element()))
        ret= true;                                // OOM
      else if ((ret= db_repository->load_named_event(thd, dbname, name,
                                                   new_element)))
        delete new_element;
      else
      {
        /*
          TODO: check if an update actually has inserted an entry
          into the queue.
          If not, and the element is ON COMPLETION NOT PRESERVE, delete
          it right away.
        */
        if (event_queue)
          event_queue->update_event(thd, parse_data->dbname, parse_data->name,
                                    new_element);
        /* Binlog the alter event. */
        DBUG_ASSERT(thd->query().str && thd->query().length);

        thd->add_to_binlog_accessed_dbs(parse_data->dbname.str);
        if (new_dbname)
          thd->add_to_binlog_accessed_dbs(new_dbname->str);

        ret= write_bin_log(thd, true, thd->query().str, thd->query().length);
      }
    }
  }
  DBUG_RETURN(ret);
}


/**
  Drops an event

  @param[in,out]  thd        THD
  @param[in]      dbname     Event's schema
  @param[in]      name       Event's name
  @param[in]      if_exists  When this is set and the event does not exist
                             a warning is pushed into the warning stack.
                             Otherwise the operation produces an error.

  @note Similarly to DROP PROCEDURE, we do not allow DROP EVENT
  under LOCK TABLES mode, unless table mysql.event is locked.  To
  ensure that, we do not reset & backup the open tables state in
  this function - if in LOCK TABLES or pre-locking mode, this will
  lead to an error 'Table mysql.event is not locked with LOCK
  TABLES' unless it _is_ locked. In pre-locked mode there is
  another barrier - DROP EVENT commits the current transaction,
  and COMMIT/ROLLBACK is not allowed in stored functions and
  triggers.

  @retval  false  OK
  @retval  true   Error (reported)
*/

bool
Events::drop_event(THD *thd, LEX_STRING dbname, LEX_STRING name, bool if_exists)
{
  int ret;
  DBUG_ENTER("Events::drop_event");

  if (check_access(thd, EVENT_ACL, dbname.str, NULL, NULL, 0, 0))
    DBUG_RETURN(true);

  // Acquire exclusive MDL lock.
  if (lock_object_name(thd, MDL_key::EVENT, dbname.str, name.str))
    DBUG_RETURN(true);

  /* On error conditions my_error() is called so no need to handle here */
  if (!(ret= db_repository->drop_event(thd, dbname, name, if_exists)))
  {
    if (event_queue)
      event_queue->drop_event(thd, dbname, name);
    /* Binlog the drop event. */
    DBUG_ASSERT(thd->query().str && thd->query().length);

    thd->add_to_binlog_accessed_dbs(dbname.str);
    ret= write_bin_log(thd, true, thd->query().str, thd->query().length);
#ifdef HAVE_PSI_SP_INTERFACE
    /* Drop statistics for this stored program from performance schema. */
    MYSQL_DROP_SP(to_uint(enum_sp_type::EVENT),
                  dbname.str, dbname.length, name.str, name.length);
#endif
  }
  DBUG_RETURN(ret);
}


/**
  Take exclusive metadata lock on all events in a schema.

  @param   thd   Thread handle.
  @param   db    Schema name.
*/

bool Events::lock_schema_events(THD *thd, const char *db)
{
  DBUG_ENTER("Events::lock_schema_events");

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  // Acquire Schema object
  const dd::Schema *sch_obj= nullptr;
  if (thd->dd_client()->acquire<dd::Schema>(db, &sch_obj))
    DBUG_RETURN(true);
  if (sch_obj == nullptr)
  {
    my_error(ER_BAD_DB_ERROR, MYF(0), db);
    DBUG_RETURN(true);
  }

  std::vector<std::string> event_names;
  if (thd->dd_client()->fetch_schema_component_names<dd::Event>(sch_obj, &event_names))
    DBUG_RETURN(true);

  MDL_request_list mdl_requests;
  for (std::vector<std::string>::const_iterator name= event_names.begin();
       name != event_names.end(); ++name)
  {
    // Event names are case insensitive, so convert to lower case.
    char lc_event_name[NAME_LEN + 1];
    convert_name_lowercase(name->c_str(), lc_event_name, sizeof(lc_event_name));

    // Add MDL_request for routine to mdl_requests list.
    MDL_request *mdl_request= new (thd->mem_root) MDL_request;
    MDL_REQUEST_INIT(mdl_request, MDL_key::EVENT, db, lc_event_name,
                     MDL_EXCLUSIVE, MDL_TRANSACTION);
    mdl_requests.push_front(mdl_request);
  }

  /* We should already hold a global IX lock and a schema X lock. */
  DBUG_ASSERT(thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::GLOBAL,
                                 "", "", MDL_INTENTION_EXCLUSIVE) &&
              thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::SCHEMA,
                                 db, "", MDL_EXCLUSIVE));
  DBUG_RETURN(thd->mdl_context.acquire_locks(&mdl_requests,
                                             thd->variables.lock_wait_timeout));

  DBUG_RETURN(false);
}


/**
  Drops all events from a schema

  @note We allow to drop all events in a schema even if the
  scheduler is disabled. This is to not produce any warnings
  in case of DROP DATABASE and a disabled scheduler.

  @param[in]      thd  THD handle.
  @param[in]      db   ASCIIZ schema name

  @returns true   drop events from database failed.
  @returns false  drop events from database succeeded.
*/
bool
Events::drop_schema_events(THD *thd, const char *db)
{
  LEX_STRING db_lex= { const_cast<char*>(db), strlen(db) };

  DBUG_ENTER("Events::drop_schema_events");
  DBUG_PRINT("enter", ("dropping events from %s", db));

  if (event_queue)
    event_queue->drop_schema_events(thd, db_lex);

  DBUG_RETURN(db_repository->drop_schema_events(thd, db_lex));
}


/**
  A helper function to generate SHOW CREATE EVENT output from a named event.

  @param[in]    thd      THD handle.
  @param[in]    et       Pointer to Event_timed object.
  @param[in]    protocol Pointer to Protocol object.
*/
static bool
send_show_create_event(THD *thd, Event_timed *et, Protocol *protocol)
{
  char show_str_buf[10 * STRING_BUFFER_USUAL_SIZE];
  String show_str(show_str_buf, sizeof(show_str_buf), system_charset_info);
  List<Item> field_list;
  LEX_STRING sql_mode;
  const String *tz_name;

  DBUG_ENTER("send_show_create_event");
  show_str.length(0);

  if (et->get_create_event(thd, &show_str))
    DBUG_RETURN(true);

  field_list.push_back(new Item_empty_string("Event", NAME_CHAR_LEN));

  if (sql_mode_string_representation(thd, et->m_sql_mode, &sql_mode))
    DBUG_RETURN(true);

  field_list.push_back(new Item_empty_string("sql_mode", (uint) sql_mode.length));

  tz_name= et->m_time_zone->get_name();

  field_list.push_back(new Item_empty_string("time_zone",
                                             tz_name->length()));

  field_list.push_back(new Item_empty_string("Create Event",
                                             show_str.length()));

  field_list.push_back(
    new Item_empty_string("character_set_client", MY_CS_NAME_SIZE));

  field_list.push_back(
    new Item_empty_string("collation_connection", MY_CS_NAME_SIZE));

  field_list.push_back(
    new Item_empty_string("Database Collation", MY_CS_NAME_SIZE));

  if (thd->send_result_metadata(&field_list,
                                Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(true);

  protocol->start_row();

  protocol->store(et->m_event_name.str, et->m_event_name.length, system_charset_info);
  protocol->store(sql_mode.str, sql_mode.length, system_charset_info);
  protocol->store(tz_name->ptr(), tz_name->length(), system_charset_info);
  protocol->store(show_str.c_ptr(), show_str.length(),
                  et->m_creation_ctx->get_client_cs());
  protocol->store(et->m_creation_ctx->get_client_cs()->csname,
                  strlen(et->m_creation_ctx->get_client_cs()->csname),
                  system_charset_info);
  protocol->store(et->m_creation_ctx->get_connection_cl()->name,
                  strlen(et->m_creation_ctx->get_connection_cl()->name),
                  system_charset_info);
  protocol->store(et->m_creation_ctx->get_db_cl()->name,
                  strlen(et->m_creation_ctx->get_db_cl()->name),
                  system_charset_info);

  if (protocol->end_row())
    DBUG_RETURN(true);

  my_eof(thd);

  DBUG_RETURN(false);
}


/**
  Implement SHOW CREATE EVENT statement

      thd   Thread context
      spn   The name of the event (db, name)

  @retval  false  OK
  @retval  true   error (reported)
*/

bool
Events::show_create_event(THD *thd, LEX_STRING dbname, LEX_STRING name)
{
  Event_timed et;
  bool ret;

  DBUG_ENTER("Events::show_create_event");
  DBUG_PRINT("enter", ("name: %s@%s", dbname.str, name.str));

  if (check_access(thd, EVENT_ACL, dbname.str, NULL, NULL, 0, 0))
    DBUG_RETURN(true);

  // We must make sure the schema is released and unlocked in the right
  // order. Fail if we are unable to get a meta data lock on the schema
  // name.
  dd::Schema_MDL_locker mdl_handler(thd);
  if (mdl_handler.ensure_locked(dbname.str))
    DBUG_RETURN(true);

  // convert event name to lower case before acquiring MDL lock.
  char event_name_buf[NAME_LEN + 1];
  convert_name_lowercase(name.str, event_name_buf, sizeof(event_name_buf));

  // Grab MDL lock on object in shared mode.
  MDL_request event_mdl_request;
  MDL_REQUEST_INIT(&event_mdl_request, MDL_key::EVENT,
                   dbname.str, event_name_buf,
                   MDL_SHARED_HIGH_PRIO, MDL_TRANSACTION);
  if (thd->mdl_context.acquire_lock(&event_mdl_request,
                                    thd->variables.lock_wait_timeout))
    DBUG_RETURN(true);

  /*
    We would like to allow SHOW CREATE EVENT under LOCK TABLES and
    in pre-locked mode. mysql.event table is marked as a system table.
    This flag reduces the set of its participation scenarios in LOCK TABLES
    operation, and therefore an out-of-bound open of this table
    for reading like the one below (sic, only for reading) is
    more or less deadlock-free. For additional information about when a
    deadlock can occur please refer to the description of 'system table'
    flag.
  */
  ret= db_repository->load_named_event(thd, dbname, name, &et);
  if (!ret)
    ret= send_show_create_event(thd, &et, thd->get_protocol());

  DBUG_RETURN(ret);
}


/**
  Check access rights and fill INFORMATION_SCHEMA.events table.

  @param[in,out]  thd     Thread context
  @param[in]      tables  The temporary table to fill.

  In MySQL INFORMATION_SCHEMA tables are temporary tables that are
  created and filled on demand. In this function, we fill
  INFORMATION_SCHEMA.events. It is a callback for I_S module, invoked from
  sql_show.cc

  @return Has to be integer, as such is the requirement of the I_S API
  @retval  0  success
  @retval  1  an error, pushed into the error stack
*/

int
Events::fill_schema_events(THD *thd, TABLE_LIST *tables, Item * /* cond */)
{
  char *db= NULL;
  int ret;
  DBUG_ENTER("Events::fill_schema_events");

  /*
    If it's SHOW EVENTS then thd->lex->select_lex->db is guaranteed not to
    be NULL. Let's do an assert anyway.
  */
  if (thd->lex->sql_command == SQLCOM_SHOW_EVENTS)
  {
    db= thd->lex->select_lex->db;
    DBUG_ASSERT(db != NULL);
    /*
      Nobody has EVENT_ACL for I_S and P_S,
      even with a GRANT ALL to *.*,
      because these schemas have additional ACL restrictions:
      see ACL_internal_schema_registry.

      Yet there are no events in I_S and P_S to hide either,
      so this check voluntarily does not enforce ACL for
      SHOW EVENTS in I_S or P_S,
      to return an empty list instead of an access denied error.

      This is more user friendly, in particular for tools.

      EVENT_ACL is not fine grained enough to differentiate:
      - creating / updating / deleting events
      - viewing existing events
    */
    if (! is_infoschema_db(db) &&
        ! is_perfschema_db(db) &&
        check_access(thd, EVENT_ACL, db, NULL, NULL, 0, 0))
      DBUG_RETURN(1);
  }
  ret= db_repository->fill_schema_events(thd, tables, db);

  DBUG_RETURN(ret);
}


/**
  Initializes the scheduler's structures.

  @param  opt_noacl_or_bootstrap
                     true if there is --skip-grant-tables or
                     --initialize. In that case we disable the event
                     scheduler.

  @note   This function is not synchronized.

  @retval  false   Perhaps there was an error, and the event scheduler
                   is disabled. But the error is not fatal and the 
                   server start up can continue.
  @retval  true    Fatal error. Startup must terminate (call unireg_abort()).
*/

bool
Events::init(my_bool opt_noacl_or_bootstrap)
{

  THD *thd;
  int err_no;
  bool res= false;

  DBUG_ENTER("Events::init");

  /*
    We need a temporary THD during boot

    Current time is stored in data member start_time of THD class
    and initialized by THD::set_time() called by ctor->THD::init()
    Subsequently, this value is used to check whether event was expired
    when make loading events from storage. Check for event expiration time
    is done at Event_queue_element::compute_next_execution_time() where
    event's status set to Event_parse_data::DISABLED and dropped flag set
    to true if event was expired.
  */
  if (!(thd= new THD()))
  {
    res= true;
    goto end;
  }
  /*
    The thread stack does not start from this function but we cannot
    guess the real value. So better some value that doesn't assert than
    no value.
  */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  /*
    We will need Event_db_repository anyway, even if the scheduler is
    disabled - to perform events DDL.
  */
  if (!(db_repository= new Event_db_repository))
  {
    res= true; /* fatal error: request unireg_abort */
    goto end;
  }

  //  If run with --skip-grant-tables or --initialize, disable the event
  //  scheduler.
  if (opt_noacl_or_bootstrap)
  {
    opt_event_scheduler= EVENTS_DISABLED;
    goto end;
  }

  DBUG_ASSERT(opt_event_scheduler == Events::EVENTS_ON ||
              opt_event_scheduler == Events::EVENTS_OFF);

  if (!(event_queue= new Event_queue) ||
      !(scheduler= new Event_scheduler(event_queue)))
  {
    res= true; /* fatal error: request unireg_abort */
    goto end;
  }

  if (event_queue->init_queue(thd) || load_events_from_db(thd, event_queue) ||
      (opt_event_scheduler == EVENTS_ON && scheduler->start(&err_no)))
  {
    sql_print_error("Event Scheduler: Error while loading from disk.");
    res= true; /* fatal error: request unireg_abort */
    goto end;
  }
  Event_worker_thread::init(db_repository);

end:
  if (res)
  {
    delete db_repository;
    db_repository= NULL;
    delete event_queue;
    event_queue= NULL;
    delete scheduler;
    scheduler= NULL;
  }
  delete thd;

  DBUG_RETURN(res);
}

/*
  Cleans up scheduler's resources. Called at server shutdown.

  SYNOPSIS
    Events::deinit()

  NOTES
    This function is not synchronized.
*/

void
Events::deinit()
{
  DBUG_ENTER("Events::deinit");

  if (opt_event_scheduler != EVENTS_DISABLED)
  {
    delete scheduler;
    scheduler= NULL;                            /* safety */
    delete event_queue;
    event_queue= NULL;                          /* safety */
  }

  delete db_repository;
  db_repository= NULL;                          /* safety */

  DBUG_VOID_RETURN;
}

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_LOCK_event_queue,
              key_event_scheduler_LOCK_scheduler_state;

static PSI_mutex_info all_events_mutexes[]=
{
  { &key_LOCK_event_queue, "LOCK_event_queue", PSI_FLAG_GLOBAL, 0},
  { &key_event_scheduler_LOCK_scheduler_state, "Event_scheduler::LOCK_scheduler_state", PSI_FLAG_GLOBAL, 0}
};

PSI_cond_key key_event_scheduler_COND_state, key_COND_queue_state;

static PSI_cond_info all_events_conds[]=
{
  { &key_event_scheduler_COND_state, "Event_scheduler::COND_state", PSI_FLAG_GLOBAL},
  { &key_COND_queue_state, "COND_queue_state", PSI_FLAG_GLOBAL},
};

PSI_thread_key key_thread_event_scheduler, key_thread_event_worker;

static PSI_thread_info all_events_threads[]=
{
  { &key_thread_event_scheduler, "event_scheduler", PSI_FLAG_GLOBAL},
  { &key_thread_event_worker, "event_worker", 0}
};
#endif /* HAVE_PSI_INTERFACE */

PSI_stage_info stage_waiting_on_empty_queue= { 0, "Waiting on empty queue", 0};
PSI_stage_info stage_waiting_for_next_activation= { 0, "Waiting for next activation", 0};
PSI_stage_info stage_waiting_for_scheduler_to_stop= { 0, "Waiting for the scheduler to stop", 0};

PSI_memory_key key_memory_event_basic_root;

#ifdef HAVE_PSI_INTERFACE
PSI_stage_info *all_events_stages[]=
{
  & stage_waiting_on_empty_queue,
  & stage_waiting_for_next_activation,
  & stage_waiting_for_scheduler_to_stop
};

static PSI_memory_info all_events_memory[]=
{
  { &key_memory_event_basic_root, "Event_basic::mem_root", PSI_FLAG_GLOBAL}
};

static void init_events_psi_keys(void)
{
  const char* category= "sql";
  int count;

  count= array_elements(all_events_mutexes);
  mysql_mutex_register(category, all_events_mutexes, count);

  count= array_elements(all_events_conds);
  mysql_cond_register(category, all_events_conds, count);

  count= array_elements(all_events_threads);
  mysql_thread_register(category, all_events_threads, count);

  count= array_elements(all_events_stages);
  mysql_stage_register(category, all_events_stages, count);

  count= array_elements(all_events_memory);
  mysql_memory_register(category, all_events_memory, count);
}
#endif /* HAVE_PSI_INTERFACE */

/*
  Inits Events mutexes

  SYNOPSIS
    Events::init_mutexes()
      thd  Thread
*/

void
Events::init_mutexes()
{
#ifdef HAVE_PSI_INTERFACE
  init_events_psi_keys();
#endif
}


/*
  Dumps the internal status of the scheduler and the memory cache
  into a table with two columns - Name & Value. Different properties
  which could be useful for debugging for instance deadlocks are
  returned.

  SYNOPSIS
    Events::dump_internal_status()
*/

void
Events::dump_internal_status()
{
  DBUG_ENTER("Events::dump_internal_status");
  puts("\n\n\nEvents status:");
  puts("LLA = Last Locked At  LUA = Last Unlocked At");
  puts("WOC = Waiting On Condition  DL = Data Locked");

  /*
    opt_event_scheduler should only be accessed while
    holding LOCK_global_system_variables.
  */
  mysql_mutex_lock(&LOCK_global_system_variables);
  if (opt_event_scheduler == EVENTS_DISABLED)
    puts("The Event Scheduler is disabled");
  else
  {
    scheduler->dump_internal_status();
    event_queue->dump_internal_status();
  }

  mysql_mutex_unlock(&LOCK_global_system_variables);
  DBUG_VOID_RETURN;
}

bool Events::start(int *err_no)
{
  bool ret= false;
  if (scheduler) ret= scheduler->start(err_no);
  return ret;
}

bool Events::stop()
{
  bool ret= false;
  if (scheduler) ret= scheduler->stop();
  return ret;
}

/**
   Loads all ENABLED events into a prioritized queue.

   This function is called during the server start up. It reads
   every event, computes the next execution time, and if the event
   needs execution, adds it to a prioritized queue. Otherwise, if
   ON COMPLETION DROP is specified, the event is automatically
   removed from the table.

   @param  thd           THD context. Used for memory allocation in some cases.
   @param  event_queue   Pointer to Event_queue object.

   @retval  false  success
   @retval  true   error, the load is aborted

   @note Reports the error to the console
*/

static bool load_events_from_db(THD *thd, Event_queue *event_queue)
{
  bool res= false;
  DBUG_ENTER("Events::load_events_from_db");
  DBUG_PRINT("enter", ("thd: 0x%lx", (long) thd));

  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());

  // Fetch all Schemas
  std::vector<const dd::Schema*> schemas;
  if (thd->dd_client()->fetch_global_components(&schemas))
    DBUG_RETURN(true);

  std::vector<std::pair<const dd::Schema*, const dd::Event*> > drop_events_vector;

  for (const dd::Schema *schema_obj : schemas)
  {
    // Fetch all events in a schema
    std::vector<const dd::Event*> events;
    if (thd->dd_client()->fetch_schema_components(schema_obj, &events))
      DBUG_RETURN(true);

    for (const dd::Event *ev_obj : events)
    {
      Event_queue_element *et= new (std::nothrow) Event_queue_element;
      if (et == nullptr)
      {
        sql_print_error("Event Scheduler: Error getting event object.");
        DBUG_RETURN(true);
      }

      if (et->fill_event_info(thd, *ev_obj, schema_obj->name().c_str()))
      {
        sql_print_error("Event Scheduler: "
                        "Error while loading events from mysql.events."
                        "The table probably contains bad data or is corrupted");
        delete et;
        res= true;
        break;
      }
      bool drop_event= et->m_dropped; // create_event may free et.
      bool created; // Not used
      if (event_queue->create_event(thd, et, &created))
      {
        /* Out of memory */
        delete et;
        DBUG_RETURN(true);
      }
      if (drop_event)
      {
        /*
          If not created, a stale event - drop if immediately if
          ON COMPLETION NOT PRESERVE.
          XXX: This won't be replicated, thus the drop won't appear in
          in the slave. When the slave is restarted it will drop events.
          However, as the slave will be "out of sync", it might happen that
          an event created on the master, after master restart, won't be
          replicated to the slave correctly, as the create will fail there.
        */
        drop_events_vector.push_back(std::make_pair(schema_obj, ev_obj));
      }
    }

    delete_container_pointers(events);
  }

  delete_container_pointers(schemas);

  for (auto event_info : drop_events_vector)
  {
    if (lock_object_name(thd, MDL_key::EVENT, event_info.first->name().c_str(),
                         event_info.second->name().c_str()))
    {
      sql_print_warning("Unable to obtain lock for ");
      sql_print_warning("dropping event %s from schema %s ",
                        event_info.second->name().c_str(),
                        event_info.first->name().c_str());
      continue;
    }
    if (dd::drop_event(thd, event_info.second))
      sql_print_warning("Unable to drop event %s from schema %s",
                        event_info.second->name().c_str(),
                        event_info.first->name().c_str());
  }
  thd->mdl_context.release_transactional_locks();
  DBUG_RETURN(res);
}

/**
  @} (End of group Event_Scheduler)
*/
