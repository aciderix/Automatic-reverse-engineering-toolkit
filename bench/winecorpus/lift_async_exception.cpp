/* Exception propagation across a thread boundary: std::async runs a task on another thread
 * (an ARET cooperative fiber); the task throws, std::future stores the exception via
 * std::promise::set_exception (std::current_exception in the worker), and future::get()
 * rethrows it in the caller (std::rethrow_exception). Exercises the exception_ptr transport
 * ACROSS fibers + the thread/future machinery (winpthread). */
#include <cstdio>
#include <future>
#include <stdexcept>

static int work(int n) {
    if (n < 0) throw std::runtime_error("negative");
    return n * n;
}

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("start\n");
    std::future<int> f1 = std::async(std::launch::async, work, 7);
    printf("f1=%d\n", f1.get());
    std::future<int> f2 = std::async(std::launch::async, work, -1);
    try {
        int r = f2.get();                 /* rethrows the exception captured in the worker */
        printf("f2=%d\n", r);
    } catch (const std::exception& e) {
        printf("f2 threw: %s\n", e.what());
    }
    printf("done\n");
    return 0;
}
