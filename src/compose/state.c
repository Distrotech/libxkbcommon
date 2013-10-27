/*
 * Copyright © 2013 Ran Benita <ran234@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "compose.h"
#include "utils.h"

struct xkb_compose_state {
    int refcnt;
    enum xkb_compose_state_flags flags;
    struct xkb_compose *compose;

    uint32_t context;
};

XKB_EXPORT struct xkb_compose_state *
xkb_compose_state_new(struct xkb_compose *compose,
                      enum xkb_compose_state_flags flags)
{
    struct xkb_compose_state *state;

    state = calloc(1, sizeof(*state));
    if (!state)
        return NULL;

    state->refcnt = 1;
    state->compose = xkb_compose_ref(compose);

    state->flags = flags;
    state->context = 0;

    return state;
}

XKB_EXPORT struct xkb_compose_state *
xkb_compose_state_ref(struct xkb_compose_state *state)
{
    state->refcnt++;
    return state;
}

XKB_EXPORT void
xkb_compose_state_unref(struct xkb_compose_state *state)
{
    if (!state || --state->refcnt > 0)
        return;

    xkb_compose_unref(state->compose);
    free(state);
}

XKB_EXPORT struct xkb_compose *
xkb_compose_state_get_compose(struct xkb_compose_state *state)
{
    return state->compose;
}

XKB_EXPORT void
xkb_compose_state_feed(struct xkb_compose_state *state, xkb_keysym_t keysym)
{
    struct xkb_compose *compose;
    uint32_t context;
    struct node *node;

    compose = state->compose;
    context = state->context;
    node = &darray_item(compose->tree, context);

    if (node->successor) {
        context = node->successor;
        node = &darray_item(compose->tree, context);
    }

    while (node->keysym != keysym && node->next != 0) {
        context = node->next;
        node = &darray_item(compose->tree, context);
    }

    if (node->keysym != keysym) {
        state->context = 0;
        return;
    }

    state->context = context;
}

XKB_EXPORT enum xkb_compose_status
xkb_compose_state_get_status(struct xkb_compose_state *state)
{
    struct node *node;

    if (state->context == 0)
        return XKB_COMPOSE_NOTHING;

    node = &darray_item(state->compose->tree, state->context);

    if (node->successor != 0)
        return XKB_COMPOSE_IN_MIDDLE;

    return XKB_COMPOSE_FOUND;
}

XKB_EXPORT int
xkb_compose_state_get_utf8(struct xkb_compose_state *state,
                           char *buffer, size_t size)
{
    struct node *node = &darray_item(state->compose->tree, state->context);
    return snprintf(buffer, size, "%s",
                    &darray_item(state->compose->utf8, node->utf8));
}

XKB_EXPORT xkb_keysym_t
xkb_compose_state_get_keysym(struct xkb_compose_state *state)
{
    struct node *node = &darray_item(state->compose->tree, state->context);
    return node->ks;
}
