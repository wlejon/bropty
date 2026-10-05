// Desktop notifications: OSC 9 (iTerm2 / ConEmu), OSC 777 (rxvt-unicode
// "notify"), and kitty's OSC 99
// (https://sw.kovidgoyal.net/kitty/desktop-notifications/):
//
//   OSC 99 ; key=value:key=value... ; payload ST
//
// Keys used: i (identifier), d (0: more chunks follow, 1: done, the
// default), p (what the payload is: title, the default, or body; ? asks
// what is supported), e (1: the payload is base64), u (urgency 0..2).
// Chunks with the same i accumulate until one has d=1; then the host gets
// the notification. Other keys (actions, occasion, icons, sounds, timeouts,
// close requests) and the payload types close / alive / icon / buttons are
// accepted and ignored: their effects belong to a notification daemon the
// host may not have, and the host learns nothing a reply would need.
#include "bropty/terminal.h"

#include "base64.h"

#include <algorithm>

namespace bropty {

namespace {

bool id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '+' || c == '.';
}

} // namespace

void Terminal::notify(Notification n) {
    if (host_) host_->notification_ex(n);
}

void Terminal::osc_notify99(std::string_view rest, bool bel) {
    const size_t semi = rest.find(';');
    std::string_view meta = rest.substr(0, semi);
    const std::string_view payload = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);

    std::string id;
    bool done = true;
    bool b64 = false;
    int urgency = -1;
    std::string_view kind = "title";
    while (!meta.empty()) {
        const size_t colon = meta.find(':');
        const std::string_view kv = meta.substr(0, colon);
        meta = colon == std::string_view::npos ? std::string_view() : meta.substr(colon + 1);
        if (kv.size() < 2 || kv[1] != '=') continue;
        const std::string_view v = kv.substr(2);
        switch (kv[0]) {
        case 'i':
            id.clear();
            for (char c : v)
                if (id_char(c)) id.push_back(c);
            break;
        case 'd': done = v != "0"; break;
        case 'e': b64 = v == "1"; break;
        case 'p': kind = v; break;
        case 'u':
            if (v.size() == 1 && v[0] >= '0' && v[0] <= '2') urgency = v[0] - '0';
            break;
        default: break;  // unknown or unsupported keys are ignored
        }
    }

    if (kind == "?") {
        // What this terminal supports: the payload types it shows and the
        // urgencies it passes on. (No actions, close events or icons.)
        reply("\x1b]99;i=" + id + ":p=?;p=?,title,body:u=0,1,2" + (bel ? "\x07" : "\x1b\\"));
        return;
    }
    const bool text = kind == "title" || kind == "body";
    // close / alive need a notification daemon to ask; icon and buttons
    // are not shown. Their chunks still count toward finishing the id.
    if (!text && kind != "icon" && kind != "buttons") return;

    auto it = std::find_if(notes_pending_.begin(), notes_pending_.end(),
                           [&](const PendingNote& p) { return p.n.id == id; });
    if (it == notes_pending_.end()) {
        if (notes_pending_.size() >= kMaxPendingNotifications) notes_pending_.erase(notes_pending_.begin());
        PendingNote p;
        p.n.id = id;
        p.n.source = "osc99";
        notes_pending_.push_back(std::move(p));
        it = notes_pending_.end() - 1;
    }
    Notification& n = it->n;
    if (urgency >= 0) n.urgency = urgency;
    if (text && !it->overflow) {
        std::string& dst = kind == "title" ? n.title : n.body;
        const size_t before = dst.size();
        if (b64) {
            if (!detail::base64_decode_append(payload, dst)) dst.resize(before);
        } else {
            dst.append(payload);
        }
        const size_t cap = std::min(kMaxNotificationBytes, opts_.max_string_bytes);
        if (n.title.size() + n.body.size() > cap) {
            // Too big: dropped whole, so its remaining chunks are swallowed
            // rather than shown as a notification of their own.
            it->overflow = true;
            std::string().swap(n.title);
            std::string().swap(n.body);
        }
    }
    if (!done) return;
    const bool dropped = it->overflow;
    Notification out = std::move(n);
    notes_pending_.erase(it);
    if (dropped) return;
    if (out.title.empty()) std::swap(out.title, out.body);  // spec: a lone body is the title
    if (out.title.empty()) return;                          // nothing to show
    notify(std::move(out));
}

} // namespace bropty
