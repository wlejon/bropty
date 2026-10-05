// OSC 52 clipboard queries answered later (TerminalHost::clipboard_read_async,
// Terminal::answer_clipboard / cancel_clipboard), through a Terminal and
// through a Session's delegate, next to the synchronous path they replace.
#include "bropty/session.h"
#include "term_helpers.h"

#include <vector>

using namespace bropty;

namespace {

// A host that takes every query and answers when told to.
struct AsyncHost : th::Capture {
    struct Query {
        uint64_t id;
        std::string selection;
    };
    std::vector<Query> queries;
    bool take{true};
    int sync_reads = 0;
    bool clipboard_read_async(uint64_t request, std::string_view selection) override {
        if (!take) return false;
        queries.push_back(Query{request, std::string(selection)});
        return true;
    }
    std::optional<std::string> clipboard_read(std::string_view s) override {
        ++sync_reads;
        return th::Capture::clipboard_read(s);
    }
};

void answered_later() {
    AsyncHost h;
    Terminal t(40, 5);
    t.set_host(&h);
    t.feed("\x1b]52;c;?\x07");          // BEL-terminated query
    t.feed("\x1b]52;;?\x1b\\");         // ST-terminated, default selection
    CHECK_EQ(h.queries.size(), size_t(2));
    CHECK_EQ(h.out, std::string(""));  // nothing answered yet
    CHECK_EQ(h.sync_reads, 0);
    CHECK_EQ(t.pending_clipboard_requests(), size_t(2));
    CHECK_EQ(h.queries[0].selection, std::string("c"));
    CHECK_EQ(h.queries[1].selection, std::string(""));
    CHECK(h.queries[0].id != h.queries[1].id);

    // Out of order, each terminated as its query was.
    CHECK(t.answer_clipboard(h.queries[1].id, "hi"));
    CHECK_EQ(h.out, std::string("\x1b]52;;aGk=\x1b\\"));
    h.out.clear();
    t.feed("more output in between\r\n");
    CHECK(t.answer_clipboard(h.queries[0].id, "data!"));
    CHECK_EQ(h.out, std::string("\x1b]52;c;ZGF0YSE=\x07"));
    // Each is answered once.
    h.out.clear();
    CHECK(!t.answer_clipboard(h.queries[0].id, "again"));
    CHECK(!t.cancel_clipboard(h.queries[1].id));
    CHECK(!t.answer_clipboard(12345, "x"));
    CHECK_EQ(h.out, std::string(""));
    CHECK_EQ(t.pending_clipboard_requests(), size_t(0));

    // Cancelled: no reply ever.
    t.feed("\x1b]52;p;?\x07");
    CHECK_EQ(h.queries.size(), size_t(3));
    CHECK(t.cancel_clipboard(h.queries[2].id));
    CHECK(!t.answer_clipboard(h.queries[2].id, "late"));
    CHECK_EQ(h.out, std::string(""));
    // An empty clipboard is an answer too.
    t.feed("\x1b]52;s0;?\x07");
    CHECK(t.answer_clipboard(h.queries[3].id, ""));
    CHECK_EQ(h.out, std::string("\x1b]52;s0;\x07"));
}

void declined_falls_back_to_sync() {
    AsyncHost h;
    h.take = false;
    h.clipboard_content = std::string("sync");
    Terminal t(40, 5);
    t.set_host(&h);
    t.feed("\x1b]52;c;?\x07");
    CHECK_EQ(h.sync_reads, 1);
    CHECK_EQ(h.out, std::string("\x1b]52;c;c3luYw==\x07"));
    CHECK_EQ(t.pending_clipboard_requests(), size_t(0));
    // The default host neither takes nor answers.
    th::Capture plain;
    Terminal u(40, 5);
    u.set_host(&plain);
    u.feed("\x1b]52;c;?\x07");
    CHECK_EQ(plain.out, std::string(""));
    CHECK_EQ(u.pending_clipboard_requests(), size_t(0));
}

void bounded() {
    AsyncHost h;
    Terminal t(40, 5);
    t.set_host(&h);
    const size_t n = Terminal::kMaxClipboardRequests + 5;
    for (size_t i = 0; i < n; ++i) t.feed("\x1b]52;c;?\x07");
    CHECK_EQ(h.queries.size(), n);
    CHECK_EQ(t.pending_clipboard_requests(), Terminal::kMaxClipboardRequests);
    // The oldest were dropped, the newest still answer.
    CHECK(!t.answer_clipboard(h.queries[0].id, "x"));
    CHECK(!t.answer_clipboard(h.queries[4].id, "x"));
    CHECK(t.answer_clipboard(h.queries[5].id, "x"));
    CHECK(t.answer_clipboard(h.queries[n - 1].id, "y"));
    // Pending queries survive RIS (the program still waits for its answer).
    t.feed("\x1b" "c");
    CHECK(t.answer_clipboard(h.queries[6].id, "z"));
}

void through_session() {
    AsyncHost h;
    Session s(th::opts(40, 5));
    s.set_delegate(&h);
    std::string to_app;
    s.set_output_callback([&](std::string_view b) { to_app.append(b); });
    s.feed("\x1b]52;c;?\x1b\\");
    CHECK_EQ(h.queries.size(), size_t(1));
    CHECK_EQ(to_app, std::string(""));
    // The answer goes to the application through the Session, like any reply.
    CHECK(s.terminal().answer_clipboard(h.queries[0].id, "pasted"));
    CHECK_EQ(to_app, std::string("\x1b]52;c;cGFzdGVk\x1b\\"));
    CHECK_EQ(h.out, std::string(""));  // the delegate is not the pty
}

} // namespace

int main() {
    init_test();
    answered_later();
    declined_falls_back_to_sync();
    bounded();
    through_session();
    return check::finish("test_clipboard");
}
