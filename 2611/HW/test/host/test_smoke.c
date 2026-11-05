#include "unity.h"
#include "esp_err.h"

void setUp(void) {}
void tearDown(void) {}

static void test_esp_err_header_usable(void)
{
    esp_err_t e = ESP_ERR_INVALID_ARG;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, e);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_esp_err_header_usable);
    return UNITY_END();
}
