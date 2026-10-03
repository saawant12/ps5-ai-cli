#include <assert.h>
#include <errno.h>
#include <time.h>

int __wrap_clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
static int sleep_error;
static int calls;
static struct timespec observed;

int ps5_test_nanosleep(const struct timespec *request, struct timespec *remaining) {
    observed = *request;
    calls++;
    if (sleep_error) {
        if (remaining) *remaining = (struct timespec){0, 7000000};
        errno = sleep_error;
        return -1;
    }
    return 0;
}

int main(void) {
    struct timespec request = {0, 25000000};
    errno = EBUSY;
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &request) == 0);
    assert(calls == 1 && observed.tv_nsec == 25000000 && errno == EBUSY);
    sleep_error = EINTR;
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, 0, &request, &request) == EINTR);
    assert(calls == 2 && request.tv_nsec == 7000000 && errno == EBUSY);
    sleep_error = EIO;
    assert(__wrap_clock_nanosleep(CLOCK_REALTIME, 0, &request, 0) == EIO);
    assert(calls == 3 && errno == EBUSY);
    request.tv_nsec = 1000000000;
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, 0, &request, 0) == EINVAL);
    request.tv_nsec = -1;
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, 0, &request, 0) == EINVAL);
    request = (struct timespec){-1, 0};
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, 0, &request, 0) == EINVAL);
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, 0, 0, 0) == EFAULT);
    request = (struct timespec){0, 1};
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &request, 0) == ENOTSUP);
    assert(__wrap_clock_nanosleep((clockid_t)-1, 0, &request, 0) == ENOTSUP);
    assert(__wrap_clock_nanosleep(CLOCK_MONOTONIC, 2, &request, 0) == EINVAL);
    assert(calls == 3 && errno == EBUSY);
    return 0;
}
