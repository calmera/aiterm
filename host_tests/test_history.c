// History budget / trimming behaviour.
#include "test_util.h"
#include "agent/history.h"
#include "util/platform.h"

static tool_call_t* one_call(const char* id) {
    tool_call_t* c = plat_calloc(1, sizeof(tool_call_t));
    c[0].id = plat_strdup(id); c[0].name = plat_strdup("t"); c[0].arguments = plat_strdup("{}");
    return c;
}

int main(void) {
    history_t h; history_init(&h, 600);
    history_add_user(&h, "u1");
    history_add_assistant(&h, NULL, one_call("c1"), 1);
    history_add_tool_result(&h, "c1", "t", "r1", false);
    history_add_assistant(&h, "a1", NULL, 0);
    CHECK_INT(h.count, 4);
    size_t before = h.bytes;
    CHECK(before < 600);
    // push a big user message -> oldest group gets dropped
    char big[400]; memset(big, 'x', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
    history_add_user(&h, big);
    CHECK(h.bytes <= 600 || h.count == 1);
    CHECK(h.head->role != MSG_TOOL);           // never start with an orphan tool result
    CHECK(h.tail->role == MSG_USER);           // newest kept
    history_clear(&h);
    CHECK_INT(h.count, 0);
    CHECK_INT(h.bytes, 0);
    // budget never drops the only message
    history_init(&h, 10);
    history_add_user(&h, "this is longer than the budget");
    CHECK_INT(h.count, 1);
    history_clear(&h);
    TEST_MAIN_END("test_history");
}
