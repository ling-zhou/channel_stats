/*
  channel_stats.cc
  channel_stats plugin for Apache Traffic Server 3.0.0+

  This plugin collect the runtime statstics (speed, request count and more in
  the future) for each channel. The stats is exposed with http interface.
  (the code of interface is from 'stats_over_http' plugin)

  Created by Conan Wang <buaawkl@gmail.com> on 2012-11-05.
*/

#define __STDC_LIMIT_MACROS

#include <cstdio>
#include <cstring>
#include <string>
// #include <ext/hash_map>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>

#include <ts/ts.h>
#include <ts/experimental.h>

#include "debug_macros.h"

#define PLUGIN_NAME     "channel_stats"
#define PLUGIN_VERSION  "0.1"

#define MAX_SPEED 999999999

static std::string api_path = "_cstats";
static TSTextLogObject log;

struct cdata {
  char * host;
};

// global stats
uint64_t global_response_count_2xx_get = 0;  // 2XX GET response count

// channel stats
struct channel_stat {
  channel_stat()
      : response_bytes_content(0), 
        response_count_2xx(0),
        speed_ua_bytes_per_sec_64k(0) {
  }

  inline void increment(uint64_t rbc, uint64_t rc2, uint64_t sbps6) {
    __sync_fetch_and_add(&response_bytes_content, rbc);
    if (rc2) __sync_fetch_and_add(&response_count_2xx, rc2);
    if (sbps6) __sync_fetch_and_add(&speed_ua_bytes_per_sec_64k, sbps6);
  }

  void debug_channel() {
    debug("response.bytes.content: %" PRIu64 "", response_bytes_content);
    debug("response.count.2xx: %" PRIu64 "", response_count_2xx);
    debug("speed.ua.bytes_per_sec_64k: %" PRIu64 "", speed_ua_bytes_per_sec_64k);
  }

  uint64_t response_bytes_content;
  uint64_t response_count_2xx;
  uint64_t speed_ua_bytes_per_sec_64k;
};

// using namespace __gnu_cxx;
// typedef __gnu_cxx::hash_map<std::string, channel_stat> stats_map_type;
typedef std::map<std::string, channel_stat *> stats_map_type;
typedef stats_map_type::const_iterator const_iterator;
typedef stats_map_type::iterator iterator;

static stats_map_type channel_stats;
static TSMutex stats_map_mutex;

// api Intercept Data

typedef struct intercept_state_t
{
  TSVConn net_vc;
  TSVIO read_vio;
  TSVIO write_vio;

  TSIOBuffer req_buffer;
  TSIOBuffer resp_buffer;
  TSIOBufferReader resp_reader;

  int output_bytes;
  int body_written;

  int show_global; // default 0
  char * channel; // default ""
  int topn; // default -1
} intercept_state;

static int handle_event(TSCont contp, TSEvent event, void *edata);
static int api_handle_event(TSCont contp, TSEvent event, void *edata);

static void
destroy_cdata(cdata * cd)
{
  if (cd) {
    TSfree(cd->host);
    TSfree(cd);
  }
}

/*
  Get the value of parameter in url querystring
  Return 0 and a null string if not find the parameter.
  Return 1 and a value string, normally
  Return 2 and a max_length value string if the length of the value exceeds.

  Possible appearance: ?param=value&fake_param=value&param=value
*/
static int
get_query_param(const char *query, const char *param, 
                char *result, int max_length)
{
  char *pos = 0;

  pos = strstr(query, param); /* try to find in querystring of url */
  if (pos != query) {
    /* if param is not prefix of querystring */
    while (pos && *(pos - 1) != '&') { /* param must be after '&' */
        pos = strstr(pos + strlen(param), param); /* try next */
    }
  }

  if (!pos) {
    /* set it null string if not found */
    result[0] = '\0';
    return 0; 
  }

  pos += strlen(param); /* skip 'param=' */

  /* copy value of param */
  int now = 0;
  while (*pos != '\0' && *pos != '&' && now < max_length) {
    result[now++] = *pos;
    pos++;
  }
  result[now] = '\0'; /* make sure null-terminated */

  if (*pos != '\0' && *pos != '&' && now == max_length)
    return 2;
  else
    return 1;
}

/*
  check if exist param in query string

  Possible querystring: ?param1=value1&param2
  (param2 is a param which "has_no_value")
*/
static int
has_query_param(const char *query, const char *param, int has_no_value)
{
  char *pos = 0;

  pos = strstr(query, param); /* try to find in querystring of url */
  if (pos != query) {
    /* if param is not prefix of querystring */
    while (pos && *(pos - 1) != '&') { /* param must be after '&' */
        pos = strstr(pos + strlen(param), param); /* try next */
    }
  }

  if (!pos)
    return 0;

  pos += strlen(param); /* skip 'param=' */

  if (has_no_value) {
    if (*pos == '\0' || *pos == '&') return 1;
  } else {
    if (*pos == '=') return 1;
  }

  return 0;
}

static void
get_api_params(TSMBuffer   bufp, 
               TSMLoc      url_loc,
               int *       show_global,
               char **     channel,
               int *       topn)
{
  const char * query; // not null-terminated, get from TS api
  char * tmp_query = NULL; // null-terminated
  int query_len = 0;
  std::stringstream ss;

  *show_global = 0;
  *topn = -1;

  query = TSUrlHttpQueryGet(bufp, url_loc, &query_len);
  if (query_len == 0)
    return;
  tmp_query = TSstrndup(query, query_len);
  debug("querystring: %s", tmp_query);

  if (has_query_param(tmp_query, "global", 1)) {
    debug("found 'global' param");
    *show_global = 1;
  }

  *channel = (char *) TSmalloc(query_len);
  if (get_query_param(tmp_query, "channel=", *channel, query_len)) {
    debug("found 'channel' param: %s", *channel);
  }

  char * tmp_topn = (char *) TSmalloc(query_len);
  if (get_query_param(tmp_query, "topn=", tmp_topn, 10)) {
    if (strlen(tmp_topn) > 0) {
      ss.str(tmp_topn);
      ss >> *topn;
    }
    debug("found 'topn' param: %d", *topn);
  }

  TSfree(tmp_query);
  TSfree(tmp_topn);
}

static void
handle_read_req(TSCont contp, TSHttpTxn txnp)
{
  TSMBuffer bufp;
  TSMLoc hdr_loc = NULL;
  TSMLoc url_loc = NULL;
  TSMLoc host_field_loc = NULL;
  const char* host_field;
  int host_field_length = 0;
  const char *method;
  int method_length = 0;
  TSCont txn_contp;

  const char * path;
  int path_len;
  cdata * cd;
  TSCont api_contp;
  intercept_state *api_state;

  if (TSHttpTxnClientReqGet(txnp, &bufp, &hdr_loc) != TS_SUCCESS) {
    error("couldn't retrieve client's request");
    goto cleanup;
  }

  method = TSHttpHdrMethodGet(bufp, hdr_loc, &method_length);
  if (0 != strncmp(method, TS_HTTP_METHOD_GET, method_length)) {
    debug("do not count %.*s method", method_length, method);
    goto cleanup;
  }

  if (TSHttpHdrUrlGet(bufp, hdr_loc, &url_loc) != TS_SUCCESS)
    goto cleanup;

  path = TSUrlPathGet(bufp, url_loc, &path_len);
  if (path_len == 0 || (unsigned)path_len != api_path.length() ||
        strncmp(api_path.c_str(), path, path_len) != 0) {
    goto not_api;
  }
  
  TSSkipRemappingSet(txnp, 1); //not strictly necessary, but speed is everything these days

  /* register our intercept */
  debug_api("Intercepting request");
  api_contp = TSContCreate(api_handle_event, TSMutexCreate());
  api_state = (intercept_state *) TSmalloc(sizeof(*api_state));
  memset(api_state, 0, sizeof(*api_state));
  get_api_params(bufp, url_loc, 
                 &api_state->show_global, &api_state->channel,
                 &api_state->topn);
  TSContDataSet(api_contp, api_state);
  TSHttpTxnIntercept(api_contp, txnp);

  goto cleanup;

not_api:

  host_field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, 
                                      TS_MIME_FIELD_HOST, TS_MIME_LEN_HOST);
  if (host_field_loc) {
    host_field = TSMimeHdrFieldValueStringGet(bufp, hdr_loc, host_field_loc, 
                                              0, &host_field_length);
  } else {
    warning("no valid host header");
    goto cleanup;
  }
  debug("origin host: %.*s", host_field_length, host_field);

  txn_contp = TSContCreate(handle_event, NULL); // reuse global hander
  cd = (cdata *) TSmalloc(sizeof(cdata));
  cd->host = TSstrndup(host_field, host_field_length);
  TSContDataSet(txn_contp, cd);

  TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, txn_contp);

cleanup:
  if (host_field_loc) TSHandleMLocRelease(bufp, hdr_loc, host_field_loc);
  if (url_loc) TSHandleMLocRelease(bufp, hdr_loc, url_loc);
  if (hdr_loc) TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
}

static void
handle_txn_close(TSCont contp, TSHttpTxn txnp)
{
  TSMBuffer bufp;
  TSMLoc hdr_loc;
  uint64_t user_speed;
  uint64_t body_bytes;
  TSHRTime start_time = 0;
  TSHRTime end_time = 0;
  TSHRTime interval_time = 0;
  iterator stat_it;
  channel_stat *stat;
  std::pair<iterator,bool> insert_ret;
  cdata * cd = (cdata *) TSContDataGet(contp);
  std::string host = std::string(cd->host);
  std::stringstream ss; // for test, to be removed

  if (TSHttpTxnClientRespGet(txnp, &bufp, &hdr_loc) != TS_SUCCESS) {
    debug("couldn't retrieve final response");
    return;
  }

  TSHttpStatus status = TSHttpHdrStatusGet(bufp, hdr_loc);
  if (status != TS_HTTP_STATUS_OK && status != TS_HTTP_STATUS_PARTIAL_CONTENT) {
    debug("only count 200/206 response");
    goto cleanup;
  }

  body_bytes = TSHttpTxnClientRespBodyBytesGet(txnp);
  TSHttpTxnStartTimeGet(txnp, &start_time);
  TSHttpTxnEndTimeGet(txnp, &end_time);
  if ((start_time != 0 && end_time != 0) || end_time < start_time) {
    interval_time = end_time - start_time;
  } else {
    error("not valid time, start: %"PRId64", end: %"PRId64"", start_time, end_time);
    goto cleanup;
  }

  if (interval_time == 0 || body_bytes == 0)
    user_speed = MAX_SPEED;
  else
    user_speed = (int)((float)body_bytes / interval_time * TS_HRTIME_SECOND);

  __sync_fetch_and_add(&global_response_count_2xx_get, 1);

  debug("origin host in ContData: %s", host.c_str());
  debug("body bytes: %" PRIu64 "", body_bytes);
  debug("start time: %" PRId64 "", start_time);
  debug("end time: %" PRId64 "", end_time);
  debug("interval time: %" PRId64 "", interval_time);
  debug("interval seconds: %.5f", interval_time / (float)TS_HRTIME_SECOND);
  debug("speed bytes per second: %" PRIu64 "", user_speed);
  debug("2xx req count: %" PRIu64 "", global_response_count_2xx_get);

  // ss << (rand() % 100);
  // host = host + "--" + ss.str();
  // debug("%s", host.c_str());
  stat_it = channel_stats.find(host);
  if (stat_it == channel_stats.end()) {
    stat = new channel_stat();
    TSMutexLock(stats_map_mutex);
    insert_ret = channel_stats.insert(std::make_pair(host, stat));
    TSMutexUnlock(stats_map_mutex);
    if (insert_ret.second == false) {
      warning("stat of this channel already existed");
      delete stat;
      stat = insert_ret.first->second;
    } else {
      debug("*********** new channel ***********");
    }
  } else { // found
    stat = stat_it->second;
  }

  stat->increment(body_bytes, 1, user_speed < 64000 ? 1 : 0);
  stat->debug_channel();

cleanup:
  TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
}

static int
handle_event(TSCont contp, TSEvent event, void *edata) {
  TSHttpTxn txnp = (TSHttpTxn) edata;

  switch (event) {
    case TS_EVENT_HTTP_READ_REQUEST_HDR: // for global contp
      debug("---------- new request ----------");
      handle_read_req(contp, txnp);
      break;
    case TS_EVENT_HTTP_TXN_CLOSE: // for txn contp
      handle_txn_close(contp, txnp);
      destroy_cdata((cdata *) TSContDataGet(contp));
      TSContDestroy(contp);
      break;
    default:
      error("unknown event for this plugin");
  }

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);

  return 0;
}


// below is api part

static void
stats_cleanup(TSCont contp, intercept_state * api_state)
{
  if (api_state->req_buffer) {
    TSIOBufferDestroy(api_state->req_buffer);
    api_state->req_buffer = NULL;
  }

  if (api_state->resp_buffer) {
    TSIOBufferDestroy(api_state->resp_buffer);
    api_state->resp_buffer = NULL;
  }

  TSfree(api_state->channel);
  TSVConnClose(api_state->net_vc);
  TSfree(api_state);
  TSContDestroy(contp);
}

static void
stats_process_accept(TSCont contp, intercept_state * api_state)
{

  api_state->req_buffer = TSIOBufferCreate();
  api_state->resp_buffer = TSIOBufferCreate();
  api_state->resp_reader = TSIOBufferReaderAlloc(api_state->resp_buffer);
  api_state->read_vio = TSVConnRead(api_state->net_vc, contp, api_state->req_buffer, INT64_MAX);
}

static int
stats_add_data_to_resp_buffer(const char *s, intercept_state * api_state)
{
  int s_len = strlen(s);

  TSIOBufferWrite(api_state->resp_buffer, s, s_len);

  return s_len;
}

static const char RESP_HEADER[] =
  "HTTP/1.0 200 Ok\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\n\r\n";

static int
stats_add_resp_header(intercept_state * api_state)
{
  return stats_add_data_to_resp_buffer(RESP_HEADER, api_state);
}

static void
stats_process_read(TSCont contp, TSEvent event, intercept_state * api_state)
{
  debug_api("stats_process_read(%d)", event);
  if (event == TS_EVENT_VCONN_READ_READY) {
    api_state->output_bytes = stats_add_resp_header(api_state);
    TSVConnShutdown(api_state->net_vc, 1, 0);
    api_state->write_vio = TSVConnWrite(api_state->net_vc, contp, api_state->resp_reader, INT64_MAX);
  } else if (event == TS_EVENT_ERROR) {
    error_api("stats_process_read: Received TS_EVENT_ERROR\n");
  } else if (event == TS_EVENT_VCONN_EOS) {
    /* client may end the connection, simply return */
    return;
  } else if (event == TS_EVENT_NET_ACCEPT_FAILED) {
    error_api("stats_process_read: Received TS_EVENT_NET_ACCEPT_FAILED\n");
  } else {
    error_api("Unexpected Event %d\n", event);
    // TSReleaseAssert(!"Unexpected Event");
  }
}

#define APPEND(a) api_state->output_bytes += stats_add_data_to_resp_buffer(a, api_state)
#define APPEND_STAT(a, fmt, v) do { \
  char b[256]; \
  if(snprintf(b, sizeof(b), "\"%s\": \"" fmt "\",\n", a, v) < (signed)sizeof(b)) \
    APPEND(b); \
} while(0)
#define APPEND_END_STAT(a, fmt, v) do { \
  char b[256]; \
  if(snprintf(b, sizeof(b), "\"%s\": \"" fmt "\"\n", a, v) < (signed)sizeof(b)) \
    APPEND(b); \
} while(0)
#define APPEND_DICT_NAME(a) do { \
  char b[256]; \
  if(snprintf(b, sizeof(b), "\"%s\": {\n", a) < (signed)sizeof(b)) \
    APPEND(b); \
} while(0)

static void
json_out_stat(TSRecordType rec_type, void *edata, int registered,
              const char *name, TSRecordDataType data_type,
              TSRecordData *datum) {
  intercept_state *api_state = (intercept_state *) edata;

  switch(data_type) {
  case TS_RECORDDATATYPE_COUNTER:
    APPEND_STAT(name, "%" PRId64, datum->rec_counter); break;
  case TS_RECORDDATATYPE_INT:
    APPEND_STAT(name, "%" PRIu64, datum->rec_int); break;
  case TS_RECORDDATATYPE_FLOAT:
    APPEND_STAT(name, "%f", datum->rec_float); break;
  case TS_RECORDDATATYPE_STRING:
    APPEND_STAT(name, "%s", datum->rec_string); break;
  default:
    debug_api("unkown type for %s: %d", name, data_type);
    break;
  }
}

template<class T>
struct compare
: std::binary_function<T,T,bool>
{
   inline bool operator()(const T& lhs, const T& rhs) {
      return lhs.second->response_count_2xx > rhs.second->response_count_2xx;
   }
};

static void
append_channel_stat(intercept_state * api_state,
                    const std::string channel, channel_stat * cs,
                    int is_last)
{
  APPEND_DICT_NAME(channel.c_str());
  APPEND_STAT("response.bytes.content", "%" PRIu64, cs->response_bytes_content);
  APPEND_STAT("response.count.2xx.get", "%" PRIu64, cs->response_count_2xx);
  APPEND_END_STAT("speed.ua.bytes_per_sec_64k", "%" PRIu64, cs->speed_ua_bytes_per_sec_64k);
  if (is_last)
    APPEND("}\n");
  else
    APPEND("},\n");
}

static void
json_out_channel_stats(intercept_state * api_state) {
  // XXX may need lock for large numbers of growing channel
  // XXX truncate result? (limit the number of outputed channel)

  if (channel_stats.empty())
    return;

  typedef std::pair<std::string, channel_stat *> data_t;
  typedef std::vector<data_t> vec_t;

  debug("appending channel stats");

  if (api_state->topn > -1 || 
      (api_state->channel && strlen(api_state->channel) > 0)) {
    // will use vector to output

    if (api_state->topn == 0)
      return;

    vec_t stats_vec; // a tmp vector to sort or filter
    if (strlen(api_state->channel) > 0) {
      // filter by channel
      size_t found;
      for (iterator it=channel_stats.begin(); it != channel_stats.end(); it++) {
        found = it->first.find(api_state->channel);
        if (found != std::string::npos)
          stats_vec.push_back(*it);
      }
    } else {
      stats_vec.assign(channel_stats.begin(), channel_stats.end());
    }

    if (stats_vec.empty())
      return;  // or out_st -1 below will overflow

    vec_t::size_type out_st = stats_vec.size();
    if (api_state->topn > 0) { // need sort and limit output size
      std::sort(stats_vec.begin(), stats_vec.end(), compare<data_t>());
      if ((unsigned)api_state->topn < stats_vec.size())
        out_st = (unsigned)api_state->topn;
    } // else will output whole vector without sort

    vec_t::size_type i = 0;
    for (; i < out_st - 1; i++) {
      append_channel_stat(api_state, stats_vec[i].first, stats_vec[i].second, 0);
    }
    append_channel_stat(api_state, stats_vec[i].first, stats_vec[i].second, 1);

  } else {
    iterator last_it = channel_stats.end();
    last_it--;
    iterator it=channel_stats.begin();
    for (; it != last_it; it++) {
      append_channel_stat(api_state, it->first, it->second, 0);
    }
    append_channel_stat(api_state, it->first, it->second, 1);
  }
}

static void
json_out_stats(intercept_state * api_state)
{
  const char *version;

  APPEND("{ \"channel\": {\n");
  json_out_channel_stats(api_state);
  APPEND("  },\n");

  APPEND(" \"global\": {\n");
  APPEND_STAT("response.count.2xx.get", "%" PRIu64, global_response_count_2xx_get);

  if (api_state->show_global)
    TSRecordDump(TS_RECORDTYPE_PROCESS, json_out_stat, api_state); // internal stats

  version = TSTrafficServerVersionGet();
  APPEND("\"server\": \"");
  APPEND(version);
  APPEND("\"\n");

  APPEND("  }\n}\n");
}

static void
stats_process_write(TSCont contp, TSEvent event, intercept_state * api_state)
{
  if (event == TS_EVENT_VCONN_WRITE_READY) {
    if (api_state->body_written == 0) {
      debug_api("plugin adding response body");
      api_state->body_written = 1;
      json_out_stats(api_state);
      TSVIONBytesSet(api_state->write_vio, api_state->output_bytes);
    }
    TSVIOReenable(api_state->write_vio);
  } else if (TS_EVENT_VCONN_WRITE_COMPLETE) {
    stats_cleanup(contp, api_state);
  } else if (event == TS_EVENT_ERROR) {
    error_api("stats_process_write: Received TS_EVENT_ERROR\n");
  } else {
    error_api("Unexpected Event %d\n", event);
    // TSReleaseAssert(!"Unexpected Event");
  }
}

static int
api_handle_event(TSCont contp, TSEvent event, void *edata)
{
  intercept_state *api_state = (intercept_state *) TSContDataGet(contp);
  if (event == TS_EVENT_NET_ACCEPT) {
    api_state->net_vc = (TSVConn) edata;
    stats_process_accept(contp, api_state);
  } else if (edata == api_state->read_vio) {
    stats_process_read(contp, event, api_state);
  } else if (edata == api_state->write_vio) {
    stats_process_write(contp, event, api_state);
  } else {
    error_api("Unexpected Event %d\n", event);
    // TSReleaseAssert(!"Unexpected Event");
  }
  return 0;
}

// initial part

static int
check_ts_version()
{
  const char *ts_version = TSTrafficServerVersionGet();
  int result = 0;

  if (ts_version) {
    int major_ts_version = 0; 
    int minor_ts_version = 0;
    int patch_ts_version = 0;

    if (sscanf(ts_version, "%d.%d.%d", &major_ts_version, &minor_ts_version,
                &patch_ts_version) != 3) {
      return 0;
    }

    /* Need at least TS 3.0.0 */
    if (major_ts_version >= 3) {
      result = 1;
    }
  }

  return result;
}

void
TSPluginInit(int argc, const char *argv[])
{
  if (argc > 2) {
    fatal("plugin does not accept more than 1 argument");
  } else if (argc == 2) {
    api_path = std::string(argv[1]);
    debug_api("stats api path: %s", api_path.c_str());
  }

  TSPluginRegistrationInfo info;

  info.plugin_name = (char *)PLUGIN_NAME;
  info.vendor_name = (char *)"wkl";
  info.support_email = (char *)"buaawkl@gmail.com";

  if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
    fatal("plugin registration failed.");
  }

  if (!check_ts_version()) {
    fatal("plugin requires Traffic Server 3.0.0 or later");
  }

  if (!log) {
    TSTextLogObjectCreate(PLUGIN_NAME, TS_LOG_MODE_ADD_TIMESTAMP, &log);
  }

  if (log) {
    TSTextLogObjectWrite(log, (char *)"%s(%s) plugin starting...",
                         PLUGIN_NAME, PLUGIN_VERSION);
  }
  info("%s(%s) plugin starting...", PLUGIN_NAME, PLUGIN_VERSION);

  stats_map_mutex = TSMutexCreate();

  TSCont cont = TSContCreate(handle_event, NULL);
  TSHttpHookAdd(TS_HTTP_READ_REQUEST_HDR_HOOK, cont);
}

