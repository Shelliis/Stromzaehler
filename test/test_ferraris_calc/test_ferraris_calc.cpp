#include <unity.h>

#include "ferraris_calc.h"

void setUp(void) {}
void tearDown(void) {}

void test_calculateWatts_typical(void)
{
  // FERRARIS_DIVIDER = 150 => ROTATION_WORTH_Wms = 60*60*1000*1000 / 150 = 2 400 000
  float rotationWorthWms = 2400000.0f;

  // 1000 ms zwischen zwei Impulsen => 2400 W
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2400.0f, calculateWatts(rotationWorthWms, 1000));
}

void test_calculateWatts_slower_rotation_means_less_power(void)
{
  float rotationWorthWms = 2400000.0f;

  // Doppelte Zeit zwischen Impulsen => halbe Leistung
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1200.0f, calculateWatts(rotationWorthWms, 2000));
}

int main(int argc, char **argv)
{
  UNITY_BEGIN();
  RUN_TEST(test_calculateWatts_typical);
  RUN_TEST(test_calculateWatts_slower_rotation_means_less_power);
  return UNITY_END();
}
