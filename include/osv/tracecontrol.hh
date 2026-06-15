#ifndef TRACECONTROL_HH
#define TRACECONTROL_HH

#include <string>
#include <vector>

class tracepoint_base;

namespace trace {

/**
 * Purposefully not including trace.hh nor making any reference
 * to types declared there. Both to keep this lean and to avoid
 * having to expand the include paths in httpserver.
 */

typedef std::string ext_id;

struct event_info {
    event_info(const tracepoint_base &);
    event_info(const event_info &) = default;

    ext_id      id;
    std::string name;
    bool        enabled;
    bool        backtrace;
};

std::vector<event_info>
get_event_info();

event_info
get_event_info(const ext_id &);

// The pattern is a wildcard expression - '*' matches any sequence, '?'
// any one character, applied anywhere in the name (substring semantics,
// like the regex-based predecessor).
std::vector<event_info>
get_event_info_matching(const std::string & wildcard);

event_info
set_event_state(const ext_id &, bool enable, bool stacktrace = false);

std::vector<event_info>
set_event_state_matching(const std::string & wildcard, bool enable, bool stacktrace = false);

event_info
set_event_state(tracepoint_base &, bool enable, bool stacktrace = false);

// (create_trace_dump() - the binary trace-dump-to-file API - was removed with
// the filesystem: it required std::ofstream and a writable temp file, and its
// only caller was the httpserver-api module.)

struct symbol {
    std::string name;
    const void * addr;
    size_t size;
    const char * filename = 0;
    uint32_t n_locations = 0;

    virtual std::pair<uint32_t, int32_t> location(uint32_t) const {
        throw std::runtime_error("no locations");
    }
};

typedef std::function<void(const symbol &)> add_symbol_func;
typedef std::function<void(const add_symbol_func &)> generate_symbol_table_func;
typedef long generator_id;

generator_id
add_symbol_callback(const generate_symbol_table_func &);

void
remove_symbol_callback(generator_id);

}

#endif // TRACECONTROL_HH
