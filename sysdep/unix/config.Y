/*
 *	BIRD -- UNIX Configuration
 *
 *	(c) 1999--2000 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

CF_HDR

#include "sysdep/unix/unix.h"
#include <stdio.h>

CF_DEFINES

static struct log_config *this_log;

CF_DECLS

CF_KEYWORDS(LOG, SYSLOG, ALL, DEBUG, TRACE, INFO, REMOTE, WARNING, ERROR, AUTH, FATAL, BUG, STDERR, SOFT)
CF_KEYWORDS(NAME, CONFIRM, UNDO, CHECK, TIMEOUT, DEBUG, LATENCY, LIMIT, WATCHDOG, WARNING, STATUS)
CF_KEYWORDS(GRACEFUL, RESTART)

%type <i> log_mask log_mask_list log_cat cfg_timeout
%type <t> cfg_name
%type <tf> timeformat_which
%type <t> syslog_name

CF_GRAMMAR

conf: log_config ;

log_begin: { this_log = cfg_allocz(sizeof(struct log_config)); };

log_config: LOG log_begin log_file log_mask ';' {
    this_log->mask = $4;
    add_tail(&new_config->logfiles, &this_log->n);
  }
 ;

syslog_name:
   NAME text { $$ = $2; }
 | { $$ = bird_name; }
 ;

log_limit:
   /* empty */
 | expr text { this_log->limit = $1; this_log->backup = $2; }
 ;

log_file:
   text log_limit {
     this_log->rf = rf_open(new_config->pool, $1, "a");
     if (!this_log->rf) cf_error("Unable to open log file '%s': %m", $1);
     this_log->fh = rf_file(this_log->rf);
     this_log->pos = -1;
     this_log->filename = $1;
   }
 | SYSLOG syslog_name { this_log->fh = NULL; new_config->syslog_name = $2; }
 | STDERR { this_log->fh = stderr; }
 ;

log_mask:
   ALL { $$ = ~0; }
 | '{' log_mask_list '}' { $$ = $2; }
 ;

log_mask_list:
   log_cat { $$ = 1 << $1; }
 | log_mask_list ',' log_cat { $$ = $1 | (1 << $3); }
 ;

log_cat:
   DEBUG { $$ = L_DEBUG[0]; }
 | TRACE { $$ = L_TRACE[0]; }
 | INFO { $$ = L_INFO[0]; }
 | REMOTE { $$ = L_REMOTE[0]; }
 | WARNING { $$ = L_WARN[0]; }
 | ERROR { $$ = L_ERR[0]; }
 | AUTH { $$ = L_AUTH[0]; }
 | FATAL { $$ = L_FATAL[0]; }
 | BUG { $$ = L_BUG[0]; }
 ;


conf: mrtdump_base ;

mrtdump_base:
   MRTDUMP PROTOCOLS mrtdump_mask ';' { new_config->proto_default_mrtdump = $3; }
 | MRTDUMP text ';' {
     struct rfile *f = rf_open(new_config->pool, $2, "a");
     if (!f) cf_error("Unable to open MRTDump file '%s': %m", $2);
     new_config->mrtdump_file = rf_fileno(f);
   }
 ;


conf: debug_unix ;

debug_unix:
   DEBUG LATENCY bool { new_config->latency_debug = $3; }
 | DEBUG LATENCY LIMIT expr_us { new_config->latency_limit = $4; }
 | WATCHDOG WARNING expr_us { new_config->watchdog_warning = $3; }
 | WATCHDOG TIMEOUT expr_us { new_config->watchdog_timeout = ($3 + 999999) TO_S; }
 ;


/* Unix specific commands */

CF_CLI_HELP(CONFIGURE, ..., [[Reload configuration]])

CF_CLI(CONFIGURE, cfg_name cfg_timeout, [\"<file>\"] [timeout [<sec>]], [[Reload configuration]])
{ cmd_reconfig($2, RECONFIG_HARD, $3); } ;

CF_CLI(CONFIGURE SOFT, cfg_name cfg_timeout, [\"<file>\"] [timeout [<sec>]], [[Reload configuration and ignore changes in filters]])
{ cmd_reconfig($3, RECONFIG_SOFT, $4); } ;

/* Hack to get input completion for 'timeout' */
CF_CLI_CMD(CONFIGURE TIMEOUT, [<sec>], [[Reload configuration with undo timeout]])
CF_CLI_CMD(CONFIGURE SOFT TIMEOUT, [<sec>], [[Reload configuration with undo timeout]])

CF_CLI(CONFIGURE CONFIRM,,, [[Confirm last configuration change - deactivate undo timeout]])
{ cmd_reconfig_confirm(); } ;

CF_CLI(CONFIGURE UNDO,,, [[Undo last configuration change]])
{ cmd_reconfig_undo(); } ;

CF_CLI(CONFIGURE STATUS,,, [[Show configuration status]])
{ cmd_reconfig_status(); } ;

CF_CLI(CONFIGURE CHECK, cfg_name, [\"<file>\"], [[Parse configuration and check its validity]])
{ cmd_check_config($3); } ;

CF_CLI(DOWN,,, [[Shut the daemon down]])
{ cmd_shutdown(); } ;

CF_CLI(GRACEFUL RESTART,,, [[Shut the daemon down for graceful restart]])
{ cmd_graceful_restart(); } ;


cfg_name:
   /* empty */ { $$ = NULL; }
 | TEXT
 ;

cfg_timeout:
   /* empty */ { $$ = 0; }
 | TIMEOUT { $$ = UNIX_DEFAULT_CONFIGURE_TIMEOUT; }
 | TIMEOUT expr { $$ = $2; }
 ;

CF_CODE

CF_END
