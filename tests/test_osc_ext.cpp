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

} // namespace

int main() {
    init_test();
    pointer_shapes();
    pointer_screens_and_host();
    return check::finish("test_osc_ext");
}
