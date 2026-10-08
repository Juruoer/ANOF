#include <anof/infrastructure/system/LinuxInstanceLock.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

int main(int argc, char** argv) {
    ::alarm(5);
    if (argc < 3) return 10;
    for (int i = 3; i < argc; ++i) {
        errno = 0;
        if (::fcntl(std::atoi(argv[i]), F_GETFD) != -1 || errno != EBADF) return 11;
    }
    auto provider = anof::infrastructure::system::makeLinuxInstanceLockProvider();
    if (!provider) return 12;
    auto lease = provider.value()->acquire(argv[1]);
    const bool expectSuccess = argv[2][0] == '1';
    if (expectSuccess) return lease && lease.value() ? 0 : 13;
    return !lease && lease.error().code == anof::ErrorCode::Conflict ? 0 : 14;
}
