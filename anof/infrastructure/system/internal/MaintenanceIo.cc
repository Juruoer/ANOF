#include <anof/infrastructure/system/internal/MaintenanceIo.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace anof::infrastructure::system::maintenance {
ssize_t Io::read(int fd, void* data, std::size_t size) { return ::read(fd, data, size); }
ssize_t Io::write(int fd, const void* data, std::size_t size) { return ::write(fd, data, size); }
int Io::sync(int fd) { return ::fsync(fd); }
int Io::truncate(int fd, off_t size) { return ::ftruncate(fd, size); }
int Io::rename(int from, const char* name, int to, const char* target) { return static_cast<int>(::syscall(SYS_renameat2, from, name, to, target, RENAME_NOREPLACE)); }
}  // namespace anof::infrastructure::system::maintenance
