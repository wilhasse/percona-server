#
# Include this script to wait until the connection to the
# server has been dropped
--disable_result_log
--disable_query_log
# Just in case, if someone enabled reconnects before waiting for disconnect,
# and happened to set the `.expect` file to "restart", not "wait", the server
# can restart in meantime before we notice it, and since the reconnect will do
# its job, we will never notice the connection is down.
--disable_reconnect
let $counter= 500;
if ($disconnect_timeout)
{
  let $counter = `SELECT $disconnect_timeout * 10`;
}
let $mysql_errno= 0;
while (!$mysql_errno)
{
  --error 0,1040,1053,1317,2002,2003,2006,2013,2016,2017,ER_CLIENT_INTERACTION_TIMEOUT
  show session status;

  dec $counter;
  if (!$counter)
  {
    if ($show_rpl_debug_info)
    {
      --source include/show_rpl_debug_info.inc
      --echo current_connectio= $CURRENT_CONNECTION
    }
    --die Server failed to dissapear
  }
  --sleep 0.1
}
--enable_query_log
--enable_result_log
