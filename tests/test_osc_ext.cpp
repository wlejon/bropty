// OSC extensions for the host: OSC 22 pointer shapes (kitty's spec and test
// sequence).
#include "term_helpers.h"

#include "bropty/session.h"

using namespace bropty;
using th::T;

namespace {

struct PointerHost : th::Capture {
    std::vector<std::string> shapes;
    void pointer_shape_changed(std::string_view n) override { shapes.emplace_back(n); }
};

void pointer_shapes() {
    T t(20, 5);
    auto send = [&](const std::string& a) {
        t.reply();
        t << "\x1b]22;" + a + "\x1b\\";
        return t.reply();
    };
    auto cur = [&] { return send("?__current__"); };
    const std::string st = "\x1b\\";

    // kitty_tests/screen.py test_pointer_shapes.
    CHECK_EQ(cur(), "\x1b]22;0" + st);
    CHECK_EQ(send("?__default__,__grabbed__,default,ne-resize,crosshair,XXX"),
             "\x1b]22;text,default,1,1,1,0" + st);
    auto step = [&](const std::string& q, const std::string& expect) {
        CHECK_EQ(send(q), std::string());
        CHECK_EQ(cur(), "\x1b]22;" + expect + st);
    };
    step("default", "default");
    t.t.reset();
    CHECK_EQ(cur(), "\x1b]22;0" + st);
    step("=crosshair", "crosshair");
    step("<", "0");
    step("=crosshair", "crosshair");
    step("", "0");
    step(">help", "help");
    step(">wait", "wait");
    step("<", "help");
    step("<", "0");
    step("default,help", "help");
    step("<", "0");
    step(">default,help", "help");
    step("<", "default");
    step("<", "0");
    step("=left_ptr", "default");
    step("=fleur", "move");
    CHECK_EQ(t.t.pointer_shape(), std::string("move"));
    // Unknown names are ignored; X11 aliases answer 0 to the support query
    // (it asks about CSS names), as kitty does.
    step("=nonsense", "move");
    CHECK_EQ(send("?fleur,move,"), "\x1b]22;0,1,0" + st);
    // BEL-terminated queries are answered with BEL.
    t << "\x1b]22;?__current__\x07";
    CHECK_EQ(t.reply(), std::string("\x1b]22;move\x07"));

    // A deep push keeps the newest 16.
    t.t.reset();
    for (int i = 0; i < 20; ++i) send(i % 2 ? ">grab" : ">wait");
    int pops = 0;
    while (cur() != "\x1b]22;0" + st && pops < 40) {
        send("<");
        ++pops;
    }
    CHECK_EQ(pops, 16);

    CHECK_EQ(pointer_shape_css_name("hand2"), std::string_view("pointer"));
    CHECK_EQ(pointer_shape_css_name("text"), std::string_view("text"));
    CHECK_EQ(pointer_shape_css_name("bogus"), std::string_view());
}

void pointer_screens_and_host() {
    PointerHost host;
    Terminal t(TerminalOptions{});
    t.set_host(&host);
    t.feed("\x1b]22;pointer\x1b\\");
    CHECK_EQ(t.pointer_shape(), std::string("pointer"));
    t.feed("\x1b]22;=pointer\x1b\\");  // unchanged: no second notice
    CHECK_EQ(host.shapes.size(), size_t(1));
    // The alternate screen has its own stack.
    t.feed("\x1b[?1049h");
    CHECK_EQ(t.pointer_shape(), std::string());
    t.feed("\x1b]22;>crosshair\x1b\\");
    CHECK_EQ(t.pointer_shape(), std::string("crosshair"));
    t.feed("\x1b[?1049l");
    CHECK_EQ(t.pointer_shape(), std::string("pointer"));
    t.feed("\x1b[?1049h");
    CHECK_EQ(t.pointer_shape(), std::string("crosshair"));
    t.feed("\x1b" "c");  // RIS empties both
    CHECK_EQ(t.pointer_shape(), std::string());
    t.feed("\x1b[?1049h");
    CHECK_EQ(t.pointer_shape(), std::string());
    const std::vector<std::string> want = {"pointer", "", "crosshair", "pointer", "crosshair", ""};
    CHECK_EQ(host.shapes.size(), want.size());
    for (size_t i = 0; i < want.size() && i < host.shapes.size(); ++i) CHECK_EQ(host.shapes[i], want[i]);

    // Configured answers to ?__default__ / ?__grabbed__.
    TerminalOptions o;
    o.default_pointer_shape = "xterm";
    o.grabbed_pointer_shape = "crosshair";
    T u(o);
    u << "\x1b]22;?__default__,__grabbed__\x07";
    CHECK_EQ(u.reply(), std::string("\x1b]22;text,crosshair\x07"));

    // Session forwards to its delegate.
    PointerHost d;
    Session s;
    s.set_delegate(&d);
    s.feed("\x1b]22;wait\x07");
    CHECK_EQ(d.shapes.size(), size_t(1));
    if (!d.shapes.empty()) CHECK_EQ(d.shapes[0], std::string("wait"));
}

struct NoteHost : bropty::TerminalHost {
    std::vector<Notification> notes;
    std::string out;
    void write_to_pty(std::string_view b) override { out.append(b); }
    void notification_ex(const Notification& n) override { notes.push_back(n); }
};

void notifications() {
    NoteHost h;
    Terminal t(TerminalOptions{});
    t.set_host(&h);
    auto last = [&]() -> Notification { return h.notes.empty() ? Notification{} : h.notes.back(); };

    // OSC 9 and OSC 777 go through notification_ex with their source.
    t.feed("\x1b]9;hello\x07");
    CHECK_EQ(h.notes.size(), size_t(1));
    CHECK_EQ(last().body, std::string("hello"));
    CHECK_EQ(last().title, std::string());
    CHECK_EQ(last().source, std::string("osc9"));
    t.feed("\x1b]9;4;1;50\x07");  // progress, not a notification
    CHECK_EQ(h.notes.size(), size_t(1));
    t.feed("\x1b]777;notify;Build;done; really\x1b\\");
    CHECK_EQ(last().title, std::string("Build"));
    CHECK_EQ(last().body, std::string("done; really"));
    CHECK_EQ(last().source, std::string("osc777"));

    // OSC 99: a bare one is a title.
    t.feed("\x1b]99;;Hello world\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(3));
    CHECK_EQ(last().title, std::string("Hello world"));
    CHECK_EQ(last().source, std::string("osc99"));
    CHECK_EQ(last().urgency, -1);
    // Chunked title + body, base64 body, urgency, unknown keys ignored, the id sanitised.
    t.feed("\x1b]99;i=a1<x>:d=0:u=2:z=whatever;Hel\x1b\\");
    t.feed("\x1b]99;i=a1x:d=0;lo\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(3));
    t.feed("\x1b]99;i=other:d=0:p=body;interleaved\x1b\\");
    t.feed("\x1b]99;i=a1x:p=body:e=1;Ym9keQ==\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(4));
    CHECK_EQ(last().id, std::string("a1x"));
    CHECK_EQ(last().title, std::string("Hello"));
    CHECK_EQ(last().body, std::string("body"));
    CHECK_EQ(last().urgency, 2);
    // The interleaved one finishes on its own; a lone body becomes the title.
    t.feed("\x1b]99;i=other:d=1:p=body;!\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(5));
    CHECK_EQ(last().title, std::string("interleaved!"));
    CHECK_EQ(last().body, std::string());
    // Nothing to show: ignored. Unsupported payload types are ignored.
    t.feed("\x1b]99;i=e;\x1b\\\x1b]99;i=x:p=close;\x1b\\\x1b]99;i=x:p=alive;\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(5));
    // An icon chunk carries no text but can finish the notification.
    t.feed("\x1b]99;i=ic:d=0;Titled\x1b\\\x1b]99;i=ic:p=icon:e=1;AAAA\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(6));
    CHECK_EQ(last().title, std::string("Titled"));

    // Query.
    t.feed("\x1b]99;i=q:p=?;\x1b\\");
    CHECK_EQ(h.out, std::string("\x1b]99;i=q:p=?;p=?,title,body:u=0,1,2\x1b\\"));
    CHECK_EQ(h.notes.size(), size_t(6));

    // Bounds: an oversized notification is dropped; too many pending ids drop the oldest.
    const std::string chunk(4000, 'x');
    for (int i = 0; i < 20; ++i) t.feed("\x1b]99;i=big:d=0;" + chunk + "\x1b\\");
    t.feed("\x1b]99;i=big;end\x1b\\");  // the oversized one is dropped, to its last chunk
    CHECK_EQ(h.notes.size(), size_t(6));
    t.feed("\x1b]99;i=big;fresh\x1b\\");  // the id is free again
    CHECK_EQ(h.notes.size(), size_t(7));
    CHECK(last().title == "fresh");
    for (size_t i = 0; i <= Terminal::kMaxPendingNotifications; ++i)
        t.feed("\x1b]99;i=p" + std::to_string(i) + ":d=0;t" + std::to_string(i) + "\x1b\\");
    t.feed("\x1b]99;i=p1;\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(8));
    CHECK(last().title == "t1");
    t.feed("\x1b]99;i=p0;\x1b\\");  // p0 was dropped: this finishes an empty one
    CHECK_EQ(h.notes.size(), size_t(8));
    // RIS drops what is pending.
    t.feed("\x1b" "c\x1b]99;i=p2;\x1b\\");
    CHECK_EQ(h.notes.size(), size_t(8));

    // A host overriding only notification() sees all three kinds.
    T old(20, 5);
    old << "\x1b]9;a\x07\x1b]777;notify;b;c\x07\x1b]99;i=1:d=0;d\x07\x1b]99;i=1:p=body;e\x07";
    CHECK_EQ(old.host.notes.size(), size_t(3));
    if (old.host.notes.size() == 3) {
        CHECK_EQ(old.host.notes[0], std::string("|a"));
        CHECK_EQ(old.host.notes[1], std::string("b|c"));
        CHECK_EQ(old.host.notes[2], std::string("d|e"));
    }

    // Session forwards notification_ex to its delegate (whose default reaches notification()).
    th::Capture d;
    Session s;
    s.set_delegate(&d);
    s.feed("\x1b]99;;hi\x07");
    CHECK_EQ(d.notes.size(), size_t(1));
    NoteHost dx;
    s.set_delegate(&dx);
    s.feed("\x1b]99;u=0;hi\x07");
    CHECK_EQ(dx.notes.size(), size_t(1));
    if (!dx.notes.empty()) CHECK_EQ(dx.notes[0].urgency, 0);
}

} // namespace

int main() {
    init_test();
    pointer_shapes();
    pointer_screens_and_host();
    notifications();
    return check::finish("test_osc_ext");
}
