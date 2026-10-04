#include "bropty/pty.h"

namespace bropty {

#if defined(_WIN32)
std::unique_ptr<IPtyProcess> create_pty_win();
#else
std::unique_ptr<IPtyProcess> create_pty_posix();
#endif

std::unique_ptr<IPtyProcess> create_pty() {
#if defined(_WIN32)
    return create_pty_win();
#else
    return create_pty_posix();
#endif
}

} // namespace bropty
