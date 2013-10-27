#include <time.h>

#include "xkbcommon/xkbcommon-compose.h"

#include "test.h"

#define BENCHMARK_ITERATIONS 200

static void
benchmark(struct xkb_context *ctx)
{
    struct timespec start, stop, elapsed;
    enum xkb_log_level old_level = xkb_context_get_log_level(ctx);
    int old_verb = xkb_context_get_log_verbosity(ctx);
    char *path;
    FILE *file;
    struct xkb_compose *compose;

    path = test_get_path("compose/Compose");
    file = fopen(path, "r");

    xkb_context_set_log_level(ctx, XKB_LOG_LEVEL_CRITICAL);
    xkb_context_set_log_verbosity(ctx, 0);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        rewind(file);
        compose = xkb_compose_new_from_file(ctx, file,
                                            XKB_COMPOSE_FORMAT_TEXT_V1,
                                            XKB_COMPOSE_COMPILE_NO_FLAGS);
        assert(compose);
        xkb_compose_unref(compose);
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);

    xkb_context_set_log_level(ctx, old_level);
    xkb_context_set_log_verbosity(ctx, old_verb);

    fclose(file);
    free(path);

    elapsed.tv_sec = stop.tv_sec - start.tv_sec;
    elapsed.tv_nsec = stop.tv_nsec - start.tv_nsec;
    if (elapsed.tv_nsec < 0) {
        elapsed.tv_nsec += 1000000000;
        elapsed.tv_sec--;
    }

    fprintf(stderr, "compiled %d compose tables in %ld.%09lds\n",
            BENCHMARK_ITERATIONS, elapsed.tv_sec, elapsed.tv_nsec);
}

static const char *
status_string(enum xkb_compose_status status)
{
    switch (status) {
    case XKB_COMPOSE_NOTHING:
        return "nothing";
    case XKB_COMPOSE_IN_MIDDLE:
        return "in-middle";
    case XKB_COMPOSE_FOUND:
        return "found";
    }

    return "<invalid-status>";
}

static bool
test_compose_seq(struct xkb_compose *compose, const char *expected_string,
                 xkb_keysym_t expected_keysym, ...)
{
    bool ret;
    struct xkb_compose_state *state;
    va_list ap;
    xkb_keysym_t keysym, ks;
    enum xkb_compose_status status, expected_status;
    char buffer[64];

    state = xkb_compose_state_new(compose, XKB_COMPOSE_STATE_NO_FLAGS);
    assert(state);

    va_start(ap, expected_keysym);

    for (int i = 1; ; i++) {
        keysym = va_arg(ap, xkb_keysym_t);
        expected_status = va_arg(ap, enum xkb_compose_status);

        xkb_compose_state_feed(state, keysym);

        status = xkb_compose_state_get_status(state);
        if (status != expected_status) {
            ret = false;
            fprintf(stderr, "after feeding %d keysyms:\n", i);
            fprintf(stderr, "expected status: %s\n", status_string(expected_status));
            fprintf(stderr, "got status: %s\n", status_string(status));
            goto out;
        }

        if (status == XKB_COMPOSE_FOUND) {
            xkb_compose_state_get_utf8(state, buffer, sizeof(buffer));
            ret = streq(buffer, expected_string);
            if (!ret) {
                fprintf(stderr, "after feeding %d keysyms:\n", i);
                fprintf(stderr, "expected string: %s\n", expected_string);
                fprintf(stderr, "got string: %s\n", buffer);
                goto out;
            }
            ks = xkb_compose_state_get_keysym(state);
            ret = (ks == expected_keysym);
            if (!ret) {
                fprintf(stderr, "after feeding %d keysyms:\n", i);
                xkb_keysym_get_name(expected_keysym, buffer, sizeof(buffer));
                fprintf(stderr, "expected keysym: %s\n", buffer);
                xkb_keysym_get_name(ks, buffer, sizeof(buffer));
                fprintf(stderr, "got keysym: %s\n", buffer);
                goto out;
            }
            break;
        }

        if (status == XKB_COMPOSE_NOTHING)
            break;
    }

    ret = true;
out:
    va_end(ap);
    xkb_compose_state_unref(state);
    return ret;
}

int
main(int argc, char *argv[])
{
    struct xkb_context *ctx;
    struct xkb_compose *compose;
    char *path;
    FILE *file;

    ctx = test_get_context(CONTEXT_NO_FLAG);
    assert(ctx);

    if (argc > 1 && streq(argv[1], "bench")) {
        benchmark(ctx);
        xkb_context_unref(ctx);
        return 0;
    }

    path = test_get_path("compose/Compose");
    file = fopen(path, "r");
    assert(file);
    free(path);

    compose = xkb_compose_new_from_file(ctx, file,
                                        XKB_COMPOSE_FORMAT_TEXT_V1,
                                        XKB_COMPOSE_COMPILE_NO_FLAGS);
    assert(compose);

    fclose(file);

    assert(test_compose_seq(compose, "~", XKB_KEY_asciitilde,
                            XKB_KEY_dead_tilde,         XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_space,              XKB_COMPOSE_FOUND));

    assert(test_compose_seq(compose, "~", XKB_KEY_asciitilde,
                            XKB_KEY_dead_tilde,         XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_dead_tilde,         XKB_COMPOSE_FOUND));

    assert(test_compose_seq(compose, "'", XKB_KEY_apostrophe,
                            XKB_KEY_dead_acute,         XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_space,              XKB_COMPOSE_FOUND));

    assert(test_compose_seq(compose, "´", XKB_KEY_acute,
                            XKB_KEY_dead_acute,         XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_dead_acute,         XKB_COMPOSE_FOUND));

    assert(test_compose_seq(compose, "´", XKB_KEY_acute,
                            XKB_KEY_Multi_key,          XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_apostrophe,         XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_apostrophe,         XKB_COMPOSE_FOUND));

    assert(test_compose_seq(compose, "", XKB_KEY_NoSymbol,
                            XKB_KEY_Multi_key,          XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_apostrophe,         XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_7,                  XKB_COMPOSE_NOTHING));

    assert(test_compose_seq(compose, "", XKB_KEY_NoSymbol,
                            XKB_KEY_Multi_key,          XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_apostrophe,         XKB_COMPOSE_IN_MIDDLE,
                            XKB_KEY_7,                  XKB_COMPOSE_NOTHING));

    assert(test_compose_seq(compose, "", XKB_KEY_NoSymbol,
                            XKB_KEY_A,                  XKB_COMPOSE_NOTHING));

    xkb_compose_unref(compose);
    xkb_context_unref(ctx);

    return 0;
}
