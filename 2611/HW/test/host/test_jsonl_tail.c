#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "jsonl_tail.h"

static char got[20][64];
static int ngot;
static bool collect(const char *l, void *ctx) { (void)ctx; snprintf(got[ngot++], 64, "%s", l); return true; }

void setUp(void) { ngot = 0; }
void tearDown(void) {}

static FILE *make(const char *content)
{
    FILE *f = tmpfile();
    fputs(content, f);
    fflush(f);
    return f;
}

static void test_reads_newest_first_with_paging(void)
{
    FILE *f = make("a1\nb22\nc333\nd4\n");
    fseek(f, 0, SEEK_END);
    long end = ftell(f), next;
    TEST_ASSERT_EQUAL_INT(2, jsonl_read_back(f, end, 2, collect, NULL, &next));
    TEST_ASSERT_EQUAL_STRING("d4", got[0]);
    TEST_ASSERT_EQUAL_STRING("c333", got[1]);
    TEST_ASSERT_TRUE(next > 0);
    TEST_ASSERT_EQUAL_INT(2, jsonl_read_back(f, next, 5, collect, NULL, &next));
    TEST_ASSERT_EQUAL_STRING("b22", got[2]);
    TEST_ASSERT_EQUAL_STRING("a1", got[3]);
    TEST_ASSERT_EQUAL_INT(0, next);
    fclose(f);
}

static void test_crosses_chunk_boundary(void)
{
    char big[2000] = "";
    for (int i = 0; i < 60; i++) {
        char l[40];
        snprintf(l, sizeof l, "{\"i\":%02d,\"pad\":\"xxxxxxxxxx\"}\n", i);
        strcat(big, l);
    }
    FILE *f = make(big);
    long next;
    TEST_ASSERT_EQUAL_INT(20, jsonl_read_back(f, (long)strlen(big), 20, collect, NULL, &next));
    TEST_ASSERT_EQUAL_STRING("{\"i\":59,\"pad\":\"xxxxxxxxxx\"}", got[0]);
    TEST_ASSERT_EQUAL_STRING("{\"i\":40,\"pad\":\"xxxxxxxxxx\"}", got[19]);
    fclose(f);
}

static void test_no_trailing_newline_and_empty(void)
{
    FILE *f = make("x\ny");
    long next;
    TEST_ASSERT_EQUAL_INT(2, jsonl_read_back(f, 3, 10, collect, NULL, &next));
    TEST_ASSERT_EQUAL_STRING("y", got[0]);
    TEST_ASSERT_EQUAL_STRING("x", got[1]);
    TEST_ASSERT_EQUAL_INT(0, next);
    TEST_ASSERT_EQUAL_INT(0, jsonl_read_back(f, 0, 10, collect, NULL, &next));
    fclose(f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reads_newest_first_with_paging);
    RUN_TEST(test_crosses_chunk_boundary);
    RUN_TEST(test_no_trailing_newline_and_empty);
    return UNITY_END();
}
