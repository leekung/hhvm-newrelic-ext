#include "hphp/runtime/ext/extension.h"
#include "hphp/runtime/ext/std/ext_std_errorfunc.h"
#include "hphp/runtime/base/php-globals.h"
#include "hphp/runtime/base/hphp-system.h"
#include "hphp/runtime/ext/hotprofiler/ext_hotprofiler.h"
#include "hphp/runtime/ext/string/ext_string.h"
#include "hphp/runtime/ext/datetime/ext_datetime.h"
#include "newrelic_transaction.h"
#include "newrelic_collector_client.h"
#include "newrelic_common.h"
#include "newrelic_profiler.h"
#include "hphp/runtime/server/transport.h"
#include "hphp/util/logger.h"

#include <string>
#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <thread>
#include <unistd.h>

#include <sys/time.h>
using namespace std;

namespace HPHP {

const StaticString
    s__SERVER("_SERVER"),
    s_REQUEST_URI("REQUEST_URI"),
    s_SCRIPT_NAME("SCRIPT_NAME"),
    s_QUERY_STRING("QUERY_STRING"),
    s_EMPTY(""),
    s_HTTP_HOST("HTTP_HOST"),
    s_HTTPS("HTTPS"),
    s_PROTO_HTTP("http://"),
    s_PROTO_HTTPS("https://");

bool keep_running = true;
bool __thread r_transaction_name = false;
int64_t __thread r_request_start = 0;

class ScopedGenericSegment : public SweepableResourceData {
public:
    DECLARE_RESOURCE_ALLOCATION(ScopedGenericSegment)
    CLASSNAME_IS("scoped_generic_segment")

    virtual const String& o_getClassNameHook() const { return classnameof(); }

    explicit ScopedGenericSegment(string name) : name(name) {
        segment_id = newrelic_segment_generic_begin(NEWRELIC_AUTOSCOPE, NEWRELIC_AUTOSCOPE, name.c_str());
    }

    virtual ~ScopedGenericSegment() {
        if (segment_id < 0) return;
        newrelic_segment_end(NEWRELIC_AUTOSCOPE, segment_id);
    }

private:
    int64_t segment_id;
    string name;
};

void ScopedGenericSegment::sweep() { }

class ScopedDatastoreSegment : public SweepableResourceData {
public:
    DECLARE_RESOURCE_ALLOCATION(ScopedDatastoreSegment)
    CLASSNAME_IS("scoped_database_segment")

    virtual const String& o_getClassNameHook() const { return classnameof(); }

    explicit ScopedDatastoreSegment(string table, string operation, string sql, string sql_trace_rollup_name) : table(table), operation(operation), sql(sql), sql_trace_rollup_name(sql_trace_rollup_name) {
        //TODO sql_trace_rollup_name
        if (sql_trace_rollup_name == "") {
            segment_id = newrelic_segment_datastore_begin(NEWRELIC_AUTOSCOPE, NEWRELIC_AUTOSCOPE, table.c_str(), operation.c_str(), sql.c_str(), NULL, NULL);
        } else {
            segment_id = newrelic_segment_datastore_begin(NEWRELIC_AUTOSCOPE, NEWRELIC_AUTOSCOPE, table.c_str(), operation.c_str(), sql.c_str(), sql_trace_rollup_name.c_str(), NULL);
        }
    }

    virtual ~ScopedDatastoreSegment() {
        if (segment_id < 0) return;
        newrelic_segment_end(NEWRELIC_AUTOSCOPE, segment_id);
    }

private:
    int64_t segment_id;
    string table;
    string operation;
    string sql;
    string sql_trace_rollup_name;
};

void ScopedDatastoreSegment::sweep() { }


class ScopedExternalSegment : public SweepableResourceData {
public:
    DECLARE_RESOURCE_ALLOCATION(ScopedExternalSegment)
    CLASSNAME_IS("scoped_external_segment")

    virtual const String& o_getClassNameHook() const { return classnameof(); }

    explicit ScopedExternalSegment(string host, string name) : host(host), name(name) {
        segment_id = newrelic_segment_external_begin(NEWRELIC_AUTOSCOPE, NEWRELIC_AUTOSCOPE, host.c_str(), name.c_str());
    }

    virtual ~ScopedExternalSegment() {
        if (segment_id < 0) return;
        newrelic_segment_end(NEWRELIC_AUTOSCOPE, segment_id);
    }

private:
    int64_t segment_id;
    string host;
    string name;
};

void ScopedExternalSegment::sweep() { }

// Profiler factory- for starting and stopping the profiler
DECLARE_EXTERN_REQUEST_LOCAL(ProfilerFactory, s_profiler_factory);

static int64_t HHVM_FUNCTION(newrelic_start_transaction_intern) {
    int64_t transaction_id = newrelic_transaction_begin();
    return transaction_id;
}

static int64_t HHVM_FUNCTION(newrelic_name_transaction_intern, const String & name) {
    r_transaction_name = true;
    // Logger::Info("Newrelic set transaction_name=%s", name.c_str());
    return newrelic_transaction_set_name(NEWRELIC_AUTOSCOPE, name.c_str());
}

static int64_t HHVM_FUNCTION(newrelic_transaction_set_request_url, const String & request_url) {
    return newrelic_transaction_set_request_url(NEWRELIC_AUTOSCOPE, request_url.c_str());
}

static int64_t HHVM_FUNCTION(newrelic_transaction_set_max_trace_segments, int threshold) {
    return newrelic_transaction_set_max_trace_segments(NEWRELIC_AUTOSCOPE, threshold);
}

static int64_t HHVM_FUNCTION(newrelic_transaction_set_threshold, int threshold) {
    //deprecated
    return false;
}

static int64_t HHVM_FUNCTION(newrelic_end_transaction) {
    return newrelic_transaction_end(NEWRELIC_AUTOSCOPE);
}

static int64_t HHVM_FUNCTION(newrelic_segment_generic_begin, const String & name) {
    return newrelic_segment_generic_begin(NEWRELIC_AUTOSCOPE, NEWRELIC_AUTOSCOPE, name.c_str());
}

static int64_t HHVM_FUNCTION(newrelic_transaction_set_category, const String & category) {
    return newrelic_transaction_set_category(NEWRELIC_AUTOSCOPE, category.c_str());
}

/* This is a raw obfuscator implementation.
 * The returned string will be freed by the NR-library, so feel free to allocate it
 */
static char * sql_obfuscator_raw(const char *sql)
{
    size_t len = strlen(sql);
    char *sql_raw = (char*)malloc(len +1);
    strncpy(sql_raw, sql, len);
    sql_raw[len] = '\0';
    // Logger::Info("NR Naked SQL: %s", sql_raw);
    return sql_raw;
}

// TODO: implements the `sql_obfuscator` to more configurable
static int64_t HHVM_FUNCTION(newrelic_segment_datastore_begin, const String & table, const String & operation, const String & sql, const String & sql_trace_rollup_name, const String & sql_obfuscator) {
    return newrelic_segment_datastore_begin(NEWRELIC_AUTOSCOPE, NEWRELIC_AUTOSCOPE, table.c_str(), operation.c_str(), sql.c_str(), sql_trace_rollup_name.c_str(), sql_obfuscator_raw);
}

static int64_t HHVM_FUNCTION(newrelic_segment_external_begin, const String & host, const String & name) {
    return newrelic_segment_external_begin(NEWRELIC_AUTOSCOPE, NEWRELIC_AUTOSCOPE, host.c_str(), name.c_str());
}

static int64_t HHVM_FUNCTION(newrelic_segment_end, int64_t id) {
    return newrelic_segment_end(NEWRELIC_AUTOSCOPE, id);
}

static int64_t HHVM_FUNCTION(newrelic_notice_error_intern, const String & exception_type, const String & error_message, const String & stack_trace, const String & stack_frame_delimiter) {
    return newrelic_transaction_notice_error(NEWRELIC_AUTOSCOPE, exception_type.c_str(), error_message.c_str(), stack_trace.c_str(), stack_frame_delimiter.c_str());
}

static int64_t HHVM_FUNCTION(newrelic_add_attribute_intern, const String & name, const String & value) {
    return newrelic_transaction_add_attribute(NEWRELIC_AUTOSCOPE, name.c_str(), value.c_str());
}

static int64_t HHVM_FUNCTION(newrelic_custom_metric, const String & name, double value) {
    return newrelic_record_metric(name.c_str(), value);
}

static void HHVM_FUNCTION(newrelic_set_external_profiler, int64_t maxdepth ) {
    Profiler *pr = new NewRelicProfiler(maxdepth);
    s_profiler_factory->setExternalProfiler(pr);
}

static Variant HHVM_FUNCTION(newrelic_get_scoped_generic_segment, const String & name) {
    // ScopedGenericSegment * segment = nullptr;
    // NEWOBJ existsonly until HHVM 3.4
    #ifdef NEWOBJ
        auto segment = NEWOBJ(ScopedGenericSegment)(name.c_str());
    // newres exists only until HHVM 3.10.0
    #elif defined newres
        auto segment = newres<ScopedGenericSegment>(name.c_str());
    #else
        auto segment = req::make<ScopedGenericSegment>(name.c_str());
    #endif
    return Resource(segment);
}

static Variant HHVM_FUNCTION(newrelic_get_scoped_database_segment, const String & table, const String & operation, const String & sql, const String & sql_trace_rollup_name) {
    // ScopedDatastoreSegment * segment = nullptr;
    // NEWOBJ existsonly until HHVM 3.4
    #ifdef NEWOBJ
        auto segment = NEWOBJ(ScopedDatastoreSegment)(table.c_str(), operation.c_str(), sql.c_str(), sql_trace_rollup_name.c_str());
    // newres exists only until HHVM 3.10.0
    #elif defined newres
        auto segment = newres<ScopedDatastoreSegment>(table.c_str(), operation.c_str(), sql.c_str(), sql_trace_rollup_name.c_str());
    #else
        auto segment = req::make<ScopedDatastoreSegment>(table.c_str(), operation.c_str(), sql.c_str(), sql_trace_rollup_name.c_str());
    #endif
    return Resource(segment);
}

static Variant HHVM_FUNCTION(newrelic_get_scoped_external_segment, const String & host, const String & name) {
    // ScopedExternalSegment * segment = nullptr;
    // NEWOBJ existsonly until HHVM 3.4
    #ifdef NEWOBJ
        auto segment = NEWOBJ(ScopedExternalSegment)(host.c_str(), name.c_str());
    // newres exists only until HHVM 3.10.0
    #elif defined newres
        auto segment = newres<ScopedExternalSegment>(host.c_str(), name.c_str());
    #else
        auto segment = req::make<ScopedExternalSegment>(host.c_str(), name.c_str());
    #endif
    return Resource(segment);
}

static bool HHVM_FUNCTION(newrelic_ignore_transaction) {
    int ret = newrelic_transaction_set_type_other(NEWRELIC_AUTOSCOPE);

    // Logger::Info("Newrelic ignore/set other");

    return ret == 0;
}

static void newrelic_status_update(int status)
{
    if (status == NEWRELIC_STATUS_CODE_SHUTDOWN)
    {
        // do something when the agent shuts down
        Logger::Info("Newrelic agent shuts down");
    }
}

static int64_t newrelic_msec_to_microsec(const String &t) {
    String s = t;
    int p = s.find('=');
    if (p > 0) {
        s = s.substr(p+1);
    }
    p = s.find('.');
    if (p > 0) {
        auto tv = HHVM_FN(str_replace)(".", "", s);
        s = tvAsCVarRef(&tv).toString();
        s += "000";
    }

    return s.toInt64();
}

// see: hphp/runtime/base/timestamp.cpp
static int64_t newrelic_current_microsec() {
    struct timeval tp;
    int64_t t;

    gettimeofday(&tp, nullptr);
    t = (tp.tv_sec * 1000000) + tp.tv_usec;

    return t;
}

const StaticString
  s_NR_ERROR_CALLBACK("NewRelicExtensionHelper::errorCallback"),
  s_NR_EXCEPTION_CALLBACK("NewRelicExtensionHelper::exceptionCallback");

static class NewRelicExtension : public Extension {
public:
    NewRelicExtension () : Extension("newrelic", NO_EXTENSION_VERSION_YET) {
        config_loaded = false;
    }

    virtual void init_newrelic() {
        newrelic_register_status_callback(newrelic_status_update);
        newrelic_register_message_handler(newrelic_message_handler);
        newrelic_init(license_key.c_str(), app_name.c_str(), app_language.c_str(), app_language_version.c_str());
    }

    virtual void moduleLoad(const IniSetting::Map& ini, Hdf config) {

        license_key = RuntimeOption::EnvVariables["NEWRELIC_LICENSE_KEY"];
        app_name = RuntimeOption::EnvVariables["NEWRELIC_APP_NAME"];
        app_language = RuntimeOption::EnvVariables["NEWRELIC_APP_LANGUAGE"];
        app_language_version = RuntimeOption::EnvVariables["NEWRELIC_APP_LANGUAGE_VERSION"];
        log_properties_file  = RuntimeOption::EnvVariables["NEWRELIC_LOG_PROPERTIES_FILE"];

        if (app_language.empty()) {
            app_language = "php-hhvm";
        }

        if (app_language_version.empty()) {
            app_language_version = HPHP::getHphpCompilerVersion();
        }


        setenv("NEWRELIC_LICENSE_KEY", license_key.c_str(), 1);
        setenv("NEWRELIC_APP_NAME", app_name.c_str(), 1);
        setenv("NEWRELIC_APP_LANGUAGE", app_language.c_str(), 1);
        setenv("NEWRELIC_APP_LANGUAGE_VERSION", app_language_version.c_str(), 1);
        setenv("NEWRELIC_LOG_PROPERTIES_FILE", log_properties_file.c_str(), 1);

        if (!license_key.empty() && !app_name.empty() && !app_language.empty() && !app_language_version.empty())
            config_loaded = true;

    }


    void moduleInit () override {
        if (config_loaded) init_newrelic();

        HHVM_FE(newrelic_start_transaction_intern);
        HHVM_FE(newrelic_name_transaction_intern);
        HHVM_FE(newrelic_transaction_set_request_url);
        HHVM_FE(newrelic_transaction_set_max_trace_segments);
        HHVM_FE(newrelic_transaction_set_threshold);
        HHVM_FE(newrelic_end_transaction);
        HHVM_FE(newrelic_segment_generic_begin);
        HHVM_FE(newrelic_segment_datastore_begin);
        HHVM_FE(newrelic_segment_external_begin);
        HHVM_FE(newrelic_segment_end);
        HHVM_FE(newrelic_get_scoped_generic_segment);
        HHVM_FE(newrelic_get_scoped_database_segment);
        HHVM_FE(newrelic_get_scoped_external_segment);
        HHVM_FE(newrelic_notice_error_intern);
        HHVM_FE(newrelic_add_attribute_intern);
        HHVM_FE(newrelic_set_external_profiler);
        HHVM_FE(newrelic_custom_metric);
        HHVM_FE(newrelic_transaction_set_category);
        HHVM_FE(newrelic_ignore_transaction);
        loadSystemlib();
    }

    virtual void requestShutdown() {
        set_info_request_uri();
        set_info_default_name();
        set_info_request_headers();
        newrelic_transaction_end(NEWRELIC_AUTOSCOPE);
    }

    void set_info_request_uri() {
        auto serverVars = php_global(s__SERVER).toArray();
        String request_url = serverVars[s_REQUEST_URI].toString();
        String https = serverVars[s_HTTPS].toString();
        String http_host = serverVars[s_HTTP_HOST].toString();
        String script_name = serverVars[s_SCRIPT_NAME].toString();
        String query_string = serverVars[s_QUERY_STRING].toString();
        String full_uri;

        if (request_url == s_EMPTY) {
            full_uri = script_name;
        } else {
            if (https == s_EMPTY) {
                full_uri = s_PROTO_HTTP;
            } else {
                full_uri = s_PROTO_HTTPS;
            }
            full_uri += http_host + request_url;
        }

        // Logger::Info("Newrelic full_uri=%s", full_uri.c_str());
        newrelic_transaction_set_request_url(NEWRELIC_AUTOSCOPE, full_uri.c_str());
        //set request_url strips query parameter, add a custom attribute with the full param
        if (query_string != s_EMPTY) {
            newrelic_transaction_add_attribute(NEWRELIC_AUTOSCOPE, "FULL_URL", full_uri.c_str());
        }
    }

    void set_info_default_name() {
        //build transaction name
        if (!r_transaction_name) {
          auto serverVars = php_global(s__SERVER).toArray();
          String request_url = serverVars[s_REQUEST_URI].toString();
          String script_name = serverVars[s_SCRIPT_NAME].toString();

          String transaction_name = request_url == s_EMPTY ? script_name : request_url;
          size_t get_param_loc = transaction_name.find('?');
          if(get_param_loc != string::npos) {
            transaction_name = transaction_name.substr(0, get_param_loc);
          }

          // Logger::Info("Newrelic default transaction_name=%s", transaction_name.c_str());
          newrelic_transaction_set_name(NEWRELIC_AUTOSCOPE, transaction_name.c_str());
        }
    }

    void set_info_request_headers() {
        // add http request headers to transaction attributes
        Transport *transport = g_context->getTransport();
        if (transport) {
            HeaderMap headers;
            transport->getHeaders(headers);
            for (auto& iter : headers) {
                const auto& values = iter.second;
                if (!values.size()) {
                    continue;
                }
                if (iter.first == "User-Agent") {
                    newrelic_transaction_add_attribute(NEWRELIC_AUTOSCOPE, "request.headers.User-Agent", values.back().c_str());
                } else if (iter.first == "Accept") {
                    newrelic_transaction_add_attribute(NEWRELIC_AUTOSCOPE, "request.headers.Accept", values.back().c_str());
                } else if (iter.first == "Accept-Language") {
                    newrelic_transaction_add_attribute(NEWRELIC_AUTOSCOPE, "request.headers.Accept-Language", values.back().c_str());
                } else if (iter.first == "Api-Version") {
                    newrelic_transaction_add_attribute(NEWRELIC_AUTOSCOPE, "request.headers.Api-Version", values.back().c_str());
                } else if (iter.first == "X-Request-Start") {
                    set_info_request_queue(values.back());
                }
            }
        }
    }

    void set_info_request_queue(const String &xRequestStart) {
        int64_t rq = r_request_start - newrelic_msec_to_microsec(xRequestStart);
        float t = (double)rq/1000000.0;

        newrelic_record_metric("WebFrontend/QueueTime", t);
    }

    void requestInit() override {
        HHVM_FN(set_error_handler)(s_NR_ERROR_CALLBACK);
        HHVM_FN(set_exception_handler)(s_NR_EXCEPTION_CALLBACK);
        //TODO: make it possible to disable that via ini

        r_transaction_name = false;
        r_request_start = newrelic_current_microsec();
        newrelic_transaction_begin();
    }

private:
    std::string license_key;
    std::string app_name;
    std::string app_language;
    std::string app_language_version;
    std::string log_properties_file;
    bool config_loaded;
} s_newrelic_extension;

HHVM_GET_MODULE(newrelic)

} // namespace HPHP
