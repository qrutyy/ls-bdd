#ifndef KUNIT_H
#define KUNIT_H

#define LSV_KTEST(name) (lsv_kunit_test_##name)
#define LSV_KUNIT_CASE(name) KUNIT_CASE(LSV_KTEST(name))
#define LSV_KUNIT_CASE_SLOW(name) KUNIT_CASE_SLOW(LSV_KTEST(name))
#endif
